// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Execute a relocated copy of the local game's actual measurement routine.
// Its two absolute jump-table operands and four table entries are relocated;
// no measurement algorithm is reimplemented. Two test stubs snapshot the same
// register ABI as Syringe and call the installed DLL's entry/exit handlers.
// Only caller identities are substituted while each callback runs; the CPU's
// real return address is restored before execution resumes.
namespace dimension_machine
{
    typedef DWORD(__cdecl* Hook)(REGISTERS*);
    static Hook entryHook, doneHook;
    static DWORD testCaller, testParent, argumentAtDone;

    static void Dispatch(REGISTERS* registers, bool done)
    {
        DWORD* frame = (DWORD*)(registers->ESP() + (done ? 0x20 : 0));
        const DWORD caller = frame[0], parent = frame[5];
        frame[0] = testCaller;
        if (testCaller == 0x433EE6) frame[5] = testParent;
        if (done) argumentAtDone = frame[1];
        (done ? doneHook : entryHook)(registers);
        frame[0] = caller; frame[5] = parent;
    }
    static void __cdecl Entry(REGISTERS* registers) { Dispatch(registers, false); }
    static void __cdecl Done(REGISTERS* registers) { Dispatch(registers, true); }
    static Hook boxEntryHook, boxDoneHook;
    static DWORD boxCaller = 0x6A9DD1;
    static void BoxDispatch(REGISTERS* registers, bool done)
    {
        DWORD* frame = (DWORD*)(registers->ESP() + (done ? 0x10 : 0));
        const DWORD caller = frame[0]; frame[0] = boxCaller;
        (done ? boxDoneHook : boxEntryHook)(registers);
        frame[0] = caller;
    }
    static void __cdecl BoxEntry(REGISTERS* registers) { BoxDispatch(registers, false); }
    static void __cdecl BoxDone(REGISTERS* registers) { BoxDispatch(registers, true); }
    static unsigned char* boxWidthRoutine;
    static void* boxFont;
    static int __fastcall BoxWidth(void* measuredFont, void*, const wchar_t* text, int)
    {
        typedef bool(__thiscall* Measure)(void*,const wchar_t*,int*,int*,int);
        const DWORD savedCaller = testCaller, savedParent = testParent;
        testCaller = 0x433EE6; testParent = 0x4A59F6;
        int width = 0;
        ((Measure)boxWidthRoutine)(measuredFont,text,&width,NULL,0);
        testCaller = savedCaller; testParent = savedParent;
        return width;
    }

    static void Jump(unsigned char* at, const void* target)
    {
        at[0] = 0xE9;
        *(DWORD*)(at + 1) = (DWORD)target - (DWORD)at - 5;
    }

    static void Stub(unsigned char* at, unsigned char* resume, const unsigned char* replay,
                     DWORD origin, void(__cdecl* dispatch)(REGISTERS*), int replayBytes = 6)
    {
        const unsigned char snapshot[] = {
            0x9C, 0x60,                         // pushfd; pushad
            0x83,0x44,0x24,0x0C,0x04,         // saved ESP += sizeof(EFLAGS)
            0x83,0xEC,0x08,                    // space for origin/flags before regs
            0x8B,0x44,0x24,0x28,              // saved EFLAGS
            0x89,0x44,0x24,0x04,
            0xC7,0x04,0x24                     // mov [esp], origin (imm32 follows)
        };
        memcpy(at, snapshot, sizeof(snapshot)); at += sizeof(snapshot);
        *(DWORD*)at = origin; at += 4;
        *at++ = 0x54;                          // push esp = REGISTERS*
        *at++ = 0xB8; *(DWORD*)at = (DWORD)dispatch; at += 4;
        *at++ = 0xFF; *at++ = 0xD0;            // call eax
        const unsigned char restore[] = {
            0x83,0xC4,0x04,
            0x8B,0x44,0x24,0x04,0x89,0x44,0x24,0x28, // propagate flags
            0x83,0xC4,0x08,0x61,0x9D           // popad; popfd
        };
        memcpy(at, restore, sizeof(restore)); at += sizeof(restore);
        memcpy(at, replay, replayBytes); at += replayBytes;
        Jump(at, resume);
    }

    static unsigned char* LoadRoutine()
    {
        FILE* file = fopen(VT_GAME_DIR "/gamemd.exe", "rb");
        if (!file) return NULL;
        fseek(file, 0, SEEK_END); const long size = ftell(file); rewind(file);
        std::vector<unsigned char> bytes(size);
        fread(bytes.data(), 1, size, file); fclose(file);
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)bytes.data();
        const IMAGE_NT_HEADERS32* pe = (const IMAGE_NT_HEADERS32*)(bytes.data() + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(pe);
        const DWORD first = 0x433CF0, last = 0x433ED0, exitOffset = 0x433E7F - first;
        DWORD offset = 0;
        for (unsigned int i = 0; i < pe->FileHeader.NumberOfSections; ++i)
        {
            const DWORD base = pe->OptionalHeader.ImageBase + section[i].VirtualAddress;
            if (first >= base && last <= base + section[i].SizeOfRawData)
                offset = section[i].PointerToRawData + first - base;
        }
        if (!offset || offset + last - first > bytes.size()) return NULL;
        unsigned char* code = (unsigned char*)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!code) return NULL;
        memcpy(code, bytes.data() + offset, last - first);
        int relocated = 0;
        for (DWORD i = 0; i + 4 <= last - first; ++i)
        {
            const DWORD value = *(DWORD*)(code + i);
            if (value >= first && value < last)
            {
                *(DWORD*)(code + i) = (DWORD)code + value - first;
                ++relocated; i += 3;
            }
        }
        const unsigned char entryBytes[] = {0x83,0xEC,0x1C,0x8B,0x41,0x04};
        const unsigned char exitBytes[] = {0xB0,0x01,0x5B,0x83,0xC4,0x1C};
        if (relocated != 6 || memcmp(code, entryBytes, 6) || memcmp(code + exitOffset, exitBytes, 6))
        {
            VirtualFree(code, 0, MEM_RELEASE); return NULL;
        }
        Stub(code + 0x240, code + 6, entryBytes, first, Entry);
        Stub(code + 0x300, code + exitOffset + 6, exitBytes, 0x433E7F, Done);
        Jump(code, code + 0x240); code[5] = 0x90;
        Jump(code + exitOffset, code + 0x300); code[exitOffset + 5] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), code, 4096);
        return code;
    }

    static void CheckBackground(HMODULE dll, void* bitFont)
    {
        boxEntryHook = (Hook)GetProcAddress(dll,"VT_Hook_Drawing_GetTextDimensions");
        boxDoneHook = (Hook)GetProcAddress(dll,"VT_Hook_Drawing_TextDimensionsDone");
        FILE* file = fopen(VT_GAME_DIR "/gamemd.exe","rb");
        CHECK(file && boxEntryHook && boxDoneHook,"native sidebar background hooks exist");
        if (!file || !boxEntryHook || !boxDoneHook) { if (file) fclose(file); return; }
        fseek(file,0,SEEK_END); const long size = ftell(file); rewind(file);
        std::vector<unsigned char> bytes(size); fread(bytes.data(),1,size,file); fclose(file);
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)bytes.data();
        const IMAGE_NT_HEADERS32* pe = (const IMAGE_NT_HEADERS32*)(bytes.data()+dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(pe);
        const DWORD first = 0x4A59E0, length = 0x6A;
        DWORD offset = 0;
        for (unsigned int i=0;i<pe->FileHeader.NumberOfSections;++i) {
            const DWORD base = pe->OptionalHeader.ImageBase + sections[i].VirtualAddress;
            if (first>=base && first+length<=base+sections[i].SizeOfRawData)
                offset = sections[i].PointerToRawData + first-base;
        }
        const unsigned char prologue[] = {0x53,0x55,0x56,0x57,0x8B,0x3D,0xD0,0xC4,0x89,0};
        const unsigned char epilogue[] = {0x89,0x7B,0x0C,0x5F,0x5E};
        CHECK(offset && !memcmp(bytes.data()+offset,prologue,10) && !memcmp(bytes.data()+offset+0x60,epilogue,5),
              "background hook instructions match actual game");
        if (!offset) return;
        unsigned char* code = (unsigned char*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        boxWidthRoutine = LoadRoutine();
        CHECK(code && boxWidthRoutine,"test copies native rectangle and width routines");
        if (!code || !boxWidthRoutine) { if (code) VirtualFree(code,0,MEM_RELEASE); if (boxWidthRoutine) VirtualFree(boxWidthRoutine,0,MEM_RELEASE); return; }
        boxFont = bitFont;
        memcpy(code,bytes.data()+offset,length);
        // Relocate the font global and route GetTextWidth to the real copied
        // measurement body. Original centring, margins and ret 14h remain.
        *(DWORD*)(code+6) = (DWORD)&boxFont;
        *(DWORD*)(code+0x12) = (DWORD)BoxWidth-(DWORD)(code+0x16);
        Stub(code+0x240,code+10,code,first,BoxEntry,10);
        Stub(code+0x300,code+0x65,epilogue,0x4A5A40,BoxDone,5);
        Jump(code,code+0x240); memset(code+5,0x90,5);
        Jump(code+0x60,code+0x300);
        FlushInstructionCache(GetCurrentProcess(),code,4096);
        typedef int*(__fastcall* BoxMeasure)(int*,const wchar_t*,int,int,int,int,int);
        BoxMeasure measure = (BoxMeasure)code;
        const int oldTracking = *(int*)((unsigned char*)bitFont+0x2C);
        const int oldHeight = *(int*)((unsigned char*)bitFont+0x1C);
        *(int*)((unsigned char*)bitFont+0x2C) = 2;
        *(int*)((unsigned char*)bitFont+0x1C) = 20;
        int rect[4] = {};
        vt::Takeover::InkY ink = {};
        const wchar_t* labels[] = { L"就绪", L"Ready" };
        for (const wchar_t* label : labels) for (int align=0;align<=2;++align) {
            const int flags=0x42|(align<<8);
            boxCaller=0x6A9DD2;
            int original[4]; measure(original,label,52,229,flags,2,1);
            boxCaller=0x6A9DD1;
            vt::Takeover::MeasureTextInkY(bitFont,label,52,align,&ink);
            CHECK(measure(rect,label,52,229,flags,2,1)==rect &&
                  rect[0]==ink.left-2 && rect[2]==ink.right-ink.left+4 &&
                  rect[1]==229+ink.top-2 && rect[3]==ink.bottom-ink.top+4,
                  "native Ready and CJK backgrounds leave 2px on all sides at every alignment; ret 14h ABI intact");
            printf("sidebar padding: align=%d width=%d height=%d, 2px each side\n",align,rect[2],rect[3]);
            boxCaller=0x6A9DD2;
            measure(rect,label,52,229,flags,2,1);
            CHECK(!memcmp(rect,original,sizeof(rect)),"adjacent sidebar caller keeps its original rectangle");
        }
        int originalTab[4];
        measure(originalTab,L"A\tB",52,229,0x142,2,1);
        boxCaller = 0x6A9DD1;
        measure(rect,L"A\tB",52,229,0x142,2,1);
        CHECK(!memcmp(rect,originalTab,sizeof(rect)),"unsupported label falls back to native rectangle");
        *(int*)((unsigned char*)bitFont+0x2C) = oldTracking;
        *(int*)((unsigned char*)bitFont+0x1C) = oldHeight;
        VirtualFree(code,0,MEM_RELEASE); VirtualFree(boxWidthRoutine,0,MEM_RELEASE);
    }

    static Hook messageHook;
    static Hook phobosMessageHook;
    static DWORD phobosMessageReturn;
    static int messageRect[4], messageFillCount;
    static int messageOpacity;
    static int __fastcall CaptureMessageFill(void*,void*,const int* rect,int color)
    {
        CHECK(color==0,"native message fill retains black background color");
        memcpy(messageRect,rect,sizeof(messageRect)); ++messageFillCount;
        return 1;
    }
    static int __fastcall CapturePhobosMessageFill(void*,void*,const int* rect,const unsigned char* color,int opacity)
    {
        CHECK(color && color[0]==0 && color[1]==0 && color[2]==0,"Phobos retains black translucent background");
        memcpy(messageRect,rect,sizeof(messageRect)); ++messageFillCount; messageOpacity=opacity;
        return 1;
    }
    static void __cdecl MessageDispatch(REGISTERS* registers) { messageHook(registers); }
    static void __cdecl PhobosMessageDispatch(REGISTERS* registers)
    {
        phobosMessageReturn=phobosMessageHook ? phobosMessageHook(registers) : 0;
        CHECK(!phobosMessageReturn || phobosMessageReturn==0x623AAB,"Phobos returns its original continuation");
    }
    // Copy only the installed Phobos handler, never load/initialize Phobos in
    // this test host. Its three global reads are redirected to controlled data;
    // the original register accesses, branches and virtual fill call execute.
    static Hook LoadPhobosMessageHandler(unsigned char* code,unsigned char* enabled,unsigned char* onNewMessages,void*** scenarioGlobal)
    {
        FILE* file=fopen(VT_GAME_DIR "/Phobos.dll","rb");
        if(!file) { printf("Phobos message compatibility: local DLL absent, skipped\n"); return NULL; }
        fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);
        std::vector<unsigned char> bytes(size);fread(bytes.data(),1,size,file);fclose(file);
        auto dos=(const IMAGE_DOS_HEADER*)bytes.data();
        auto pe=(const IMAGE_NT_HEADERS32*)(bytes.data()+dos->e_lfanew);
        auto sections=IMAGE_FIRST_SECTION(pe);
        auto offset=[&](DWORD rva)->DWORD {
            for(unsigned i=0;i<pe->FileHeader.NumberOfSections;++i)
                if(rva>=sections[i].VirtualAddress && rva<sections[i].VirtualAddress+sections[i].SizeOfRawData)
                    return sections[i].PointerToRawData+rva-sections[i].VirtualAddress;
            return 0;
        };
        auto exports=(const IMAGE_EXPORT_DIRECTORY*)(bytes.data()+offset(pe->OptionalHeader.DataDirectory[0].VirtualAddress));
        auto names=(const DWORD*)(bytes.data()+offset(exports->AddressOfNames));
        auto ordinals=(const WORD*)(bytes.data()+offset(exports->AddressOfNameOrdinals));
        auto functions=(const DWORD*)(bytes.data()+offset(exports->AddressOfFunctions));
        DWORD handler=0;
        for(DWORD i=0;i<exports->NumberOfNames;++i)
            if(!strcmp((const char*)bytes.data()+offset(names[i]),"DSurface_sub_623880_DrawBitFontStrings"))
                handler=offset(functions[ordinals[i]]);
        CHECK(handler && handler+100<=bytes.size(),"installed Phobos message handler located by export");
        if(!handler || handler+100>bytes.size())return NULL;
        const unsigned char* source=bytes.data()+handler;
        // Version guard: exactly this handler's 100-byte body and global operands.
        const unsigned char tail[]={0x8B,0x06,0x51,0x8D,0x4C,0x24,0x08,0x51,0x52,0x8B,0xCE,0xFF,0x50,0x1C,
                                    0xB8,0xAB,0x3A,0x62,0x00,0x5E,0x59,0xC3};
        bool known=source[0]==0x51 && source[1]==0x80 && source[2]==0x3D &&
            source[31]==0x80 && source[32]==0x3D && source[52]==0xA1 && !memcmp(source+78,tail,sizeof(tail));
        CHECK(known,"installed Phobos handler shape matches isolated fixture");
        if(!known)return NULL;
        memcpy(code,source,100);
        *(DWORD*)(code+3)=(DWORD)enabled;*(DWORD*)(code+33)=(DWORD)onNewMessages;*(DWORD*)(code+53)=(DWORD)scenarioGlobal;
        return (Hook)code;
    }
    static void CheckMessageBackground(HMODULE dll,void* bitFont,const wchar_t* text,unsigned char* measurementCode)
    {
        messageHook=(Hook)GetProcAddress(dll,"VT_Hook_Message_Background");
        FILE* file=fopen(VT_GAME_DIR "/gamemd.exe","rb");
        CHECK(messageHook && file,"message background handler and native binary available");
        if(!messageHook || !file) { if(file)fclose(file); return; }
        fseek(file,0,SEEK_END); long size=ftell(file); rewind(file);
        std::vector<unsigned char> bytes(size); fread(bytes.data(),1,size,file); fclose(file);
        auto dos=(const IMAGE_DOS_HEADER*)bytes.data();
        auto pe=(const IMAGE_NT_HEADERS32*)(bytes.data()+dos->e_lfanew);
        auto section=IMAGE_FIRST_SECTION(pe); DWORD offset=0;
        for(unsigned i=0;i<pe->FileHeader.NumberOfSections;++i) {
            DWORD base=pe->OptionalHeader.ImageBase+section[i].VirtualAddress;
            if(0x623A97>=base && 0x623AAB<=base+section[i].SizeOfRawData)
                offset=section[i].PointerToRawData+0x623A97-base;
        }
        const unsigned char expected[]={0x89,0x54,0x24,0x30,0x8D,0x44,0x24,0x30,0x8B,0x11,0x6A,0x00,0x50,0x89,0x6C,0x24,0x44,0xFF,0x52,0x14};
        CHECK(offset && !memcmp(bytes.data()+offset,expected,sizeof(expected)),"native message rectangle write and fill-call bytes verified");
        if(!offset || memcmp(bytes.data()+offset,expected,sizeof(expected)))return;
        unsigned char* code=(unsigned char*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        CHECK(code,"native message fixture allocated"); if(!code)return;
        void* table[8]={};table[5]=(void*)CaptureMessageFill;table[7]=(void*)CapturePhobosMessageFill;void** surface=table;
        int point[4]={292,796,400,20};
        unsigned char* at=code;
        *at++=0x53;*at++=0x55;*at++=0x56;*at++=0x57; // preserve native callee-saved registers
        *at++=0x81;*at++=0xEC;*(DWORD*)at=0x1060;at+=4;
        auto movStack=[&](DWORD slot,DWORD value) {
            *at++=0xC7;*at++=0x84;*at++=0x24;*(DWORD*)at=slot;at+=4;*(DWORD*)at=value;at+=4;
        };
        movStack(0x104C,(DWORD)bitFont);
        movStack(0x30,291);movStack(0x34,795);movStack(0x38,400);
        *at++=0xB9;*(DWORD*)at=(DWORD)&surface;at+=4;
        *at++=0xBE;*(DWORD*)at=(DWORD)point;at+=4;
        *at++=0xBF;*(DWORD*)at=(DWORD)text;at+=4;
        *at++=0xBD;*(DWORD*)at=22;at+=4;
        *at++=0xBA;*(DWORD*)at=291;at+=4; // native EDX supplies background X
        unsigned char* body=at;memcpy(at,bytes.data()+offset,sizeof(expected));at+=sizeof(expected);
        *at++=0x81;*at++=0xC4;*(DWORD*)at=0x1060;at+=4;
        *at++=0x5F;*at++=0x5E;*at++=0x5D;*at++=0x5B;*at++=0xC3;
        Stub(code+0x300,body+8,expected,0x623A97,MessageDispatch,8);
        Jump(body,code+0x300);
        // Reproduce Syringe's Phobos return-zero/native or nonzero/skip split.
        Stub(code+0x400,code+0x600,expected,0x623A9F,PhobosMessageDispatch,0);
        Jump(body+8,code+0x400);
        at=code+0x600;
        *at++=0x83;*at++=0x3D;*(DWORD*)at=(DWORD)&phobosMessageReturn;at+=4;*at++=0;
        *at++=0x0F;*at++=0x85;*(DWORD*)at=(DWORD)(body+20)-(DWORD)(at+4);at+=4;
        memcpy(at,expected+8,5);at+=5;Jump(at,body+13);
        FlushInstructionCache(GetCurrentProcess(),code,4096);
        typedef bool(__thiscall* Measure)(void*,const wchar_t*,int*,int*,int);
        int width=0,height=0;
        vt::Takeover::InkY ink={};
        vt::Takeover::MeasureTextInkY(bitFont,text,0,0,&ink);
        testCaller=0x433EE6;testParent=0x623A81;
        CHECK(((Measure)measurementCode)(bitFont,text,&width,&height,0),"message measurement capture completed by actual native body");
        messageFillCount=0;((void(__cdecl*)())code)();
        CHECK(messageFillCount==1 && messageRect[0]==point[0]+ink.left-2 && messageRect[2]==ink.right-ink.left+4 &&
              messageRect[1]==point[1]+ink.top-1 && messageRect[3]==ink.bottom-ink.top+3 && point[1]==796 && point[3]==20,
              "native message fill leaves 2px horizontally, 1px above and 2px below while text position stays intact");
        const int bottom=messageRect[1]+messageRect[3];
        point[1]+=19;
        ((Measure)measurementCode)(bitFont,text,&width,&height,0);
        ((void(__cdecl*)())code)();
        CHECK(messageRect[1]==bottom,"consecutive 19px task rows join exactly without overlap");
        ((void(__cdecl*)())code)();
        CHECK(messageRect[1]==795 && messageRect[3]==22,"message capture is consumed once; absent capture keeps native rectangle");
        printf("task message: background height 22 -> %d, native 19px row spacing preserved\n",ink.bottom-ink.top+3);
        unsigned char enabled=1,onNewMessages=0;
        std::vector<unsigned char> scenarioBytes(0x35A3);void* scenario=scenarioBytes.data();void** scenarioGlobal=&scenario;
        phobosMessageHook=LoadPhobosMessageHandler(code+0x500,&enabled,&onNewMessages,&scenarioGlobal);
        FlushInstructionCache(GetCurrentProcess(),code,4096);
        if(phobosMessageHook) {
            for(int mode=0;mode<4;++mode) {
                enabled=mode!=3;onNewMessages=mode==2;scenarioBytes[0x35A2]=mode==1;
                for(int row=0;row<2;++row) {
                    point[1]=796+19*row;
                    ((Measure)measurementCode)(bitFont,text,&width,&height,0);
                    messageFillCount=0;messageOpacity=-1;((void(__cdecl*)())code)();
                    CHECK(messageFillCount==1 && messageRect[0]==point[0]+ink.left-2 && messageRect[2]==ink.right-ink.left+4 &&
                        messageRect[1]==point[1]+ink.top-1 && messageRect[3]==ink.bottom-ink.top+3 && point[3]==20,
                        "installed Phobos handler receives compact rectangle, fills once, preserves native row metrics");
                    CHECK((mode==3 && phobosMessageReturn==0 && messageOpacity==-1) ||
                        (mode<3 && phobosMessageReturn==0x623AAB && messageOpacity==(mode==0?40:70)),
                        "Phobos enabled/disabled paths retain native transparency and continuation");
                    if(row==1)CHECK(messageRect[1]==bottom,"Phobos consecutive task rows join exactly without overlap");
                }
            }
            printf("Phobos installed handler: 40/70 translucent fill and disabled fallback passed, 19px rows do not overlap\n");
        }
        phobosMessageHook=NULL;
        VirtualFree(code,0,MEM_RELEASE);
    }

    static void Check(HMODULE dll, void* bitFont)
    {
        entryHook = (Hook)GetProcAddress(dll, "VT_Hook_BitFont_GetTextDimension");
        doneHook = (Hook)GetProcAddress(dll, "VT_Hook_BitFont_DimensionDone");
        unsigned char* code = LoadRoutine();
        CHECK(code && entryHook && doneHook, "local game measurement machine code and both handlers loaded");
        if (!code || !entryHook || !doneHook) return;
        typedef bool(__thiscall* Measure)(void*, const wchar_t*, int*, int*, int);
        Measure measure = (Measure)code;
        const int oldHeight = *(int*)((unsigned char*)bitFont + 0x1C);
        *(int*)((unsigned char*)bitFont + 0x1C) = 20;
        const wchar_t* objectives = L"任务目标一： 保护天气控制机\n任务目标二： 消灭敌军部队";
        int width = 0, height = 0, expected = 0;
        vt::Takeover::MeasureDynamicWidth(bitFont, objectives, 400, &expected);
        testCaller = 0x553199; testParent = 0; argumentAtDone = 0;
        CHECK(measure(bitFont, objectives, &width, &height, 400), "original machine code returns success through real ret 10h");
        CHECK(argumentAtDone == 190, "original machine code overwrites text argument with 190px before exit hook");
        CHECK(width == expected && height == 40, "actual engine body plus installed hooks gives 208px and original 40px height");
        printf("native measurement: engine accumulator=%lu width=%d height=%d\n", argumentAtDone, width, height);
        const wchar_t* message = L"我们很快就会从三个不同的方向对尤里的巨塔发起攻击。";
        vt::Takeover::MeasureDynamicWidth(bitFont, message, 0, &expected);
        testCaller = 0x433EE6; testParent = 0x623A81;
        CHECK(measure(bitFont, message, &width, &height, 0) && width == expected,
            "actual engine body plus message-background caller gets natural width with padding");
        CheckMessageBackground(dll,bitFont,message,code);
        testCaller=0x433EE6;
        testParent = 0x5D4706;
        CHECK(measure(bitFont, message, &width, &height, 0) && width == LegacyWidth(message),
            "actual engine body keeps original wrapping-probe width");
        const wchar_t* tooltip = L"军械库\n-----------\n解锁科技\n技能:冰锥\n$800 \u231A00:41 \u26A1-50";
        CHECK(vt::Takeover::MeasureDynamicWidth(bitFont,tooltip,168,&expected),
            "mixed tooltip explicit lines fit sidebar surface with natural padding");
        testCaller=0x4A5EF1;
        CHECK(measure(bitFont,tooltip,&width,&height,168),"native tooltip baseline measurement succeeds");
        const int tooltipLegacy=width,tooltipHeight=height;
        vt::Takeover::InkY ink = {};
        CHECK(vt::Takeover::MeasureTextInkY(bitFont,tooltip,0,0,&ink),"mixed tooltip full vertical bounds measured");
        testCaller=0x478F0B;
        CHECK(measure(bitFont,tooltip,&width,&height,168) &&
              width==(expected>tooltipLegacy?expected:tooltipLegacy) && height==ink.bottom-ink.top,
              "native tooltip caller keeps width minimum and fits complete vertical ink");
        CHECK(ink.lines==5 && height<tooltipHeight,"tooltip trims outer cell slack without removing a row");
        printf("tooltip: width legacy=%d new=%d; height legacy=%d ink=%d plus-native-padding=4\n",tooltipLegacy,width,tooltipHeight,height);
        testCaller=0x478F0C;
        CHECK(measure(bitFont,tooltip,&width,&height,168) && width==tooltipLegacy && height==tooltipHeight,
              "unrelated adjacent caller is not treated as tooltip");
        const wchar_t* narrowTooltip = L"-----------\nOK";
        testCaller = 0x4A5EF1;
        CHECK(measure(bitFont,narrowTooltip,&width,&height,168), "narrow tooltip legacy measurement");
        const int oldNarrow = width, oldNarrowHeight = height;
        vt::Takeover::MeasureDynamicWidth(bitFont,narrowTooltip,168,&expected);
        vt::Takeover::MeasureTextInkY(bitFont,narrowTooltip,0,0,&ink);
        testCaller = 0x478F0B;
        CHECK(measure(bitFont,narrowTooltip,&width,&height,168) && width == oldNarrow &&
              height == ink.bottom-ink.top && height < oldNarrowHeight, "tooltip shrinks vertically and keeps legacy width minimum");
        typedef DWORD(__cdecl* Hook)(REGISTERS*);
        Hook drawTooltip = (Hook)GetProcAddress(dll, "VT_Hook_BitText_DrawText");
        DWORD tooltipFrame[11] = { 0x479041, (DWORD)bitFont, 0, (DWORD)narrowTooltip, 100, 12, (DWORD)(width + 8), (DWORD)(height+4) };
        REGISTERS tooltipRegs = {}; tooltipRegs.esp = (DWORD)tooltipFrame;
        CHECK(drawTooltip && drawTooltip(&tooltipRegs) == 0 && tooltipFrame[4]==100 &&
              tooltipFrame[5]==(DWORD)(12-ink.top) && tooltipFrame[6]==(DWORD)(width+8) &&
              tooltipFrame[7]==(DWORD)(oldNarrowHeight+4),
              "tooltip text fits ink into 2px gutters and retains native row budget, X and width");
        tooltipFrame[5] = 12; tooltipFrame[7] = height+4;
        CHECK(drawTooltip(&tooltipRegs) == 0 && tooltipFrame[5]==12 && tooltipFrame[7]==(DWORD)(height+4),
              "tooltip vertical capture is consumed once");
        printf("compact tooltip: width=%d unchanged; height=%d -> %d plus-native-padding=4; ink-top=%d\n",
               oldNarrow,oldNarrowHeight,height,ink.top);
        testCaller=0x4A5EF1;
        CHECK(measure(bitFont,tooltip,&width,&height,32),"automatically wrapped tooltip original measurement");
        const int wrappedWidth=width, wrappedHeight=height;
        testCaller=0x478F0B;
        CHECK(measure(bitFont,tooltip,&width,&height,32) && width==wrappedWidth && height==wrappedHeight,
              "automatically wrapped tooltip keeps native width and height");
        testCaller = 0x553199;
        CHECK(!measure(bitFont, NULL, &width, &height, 400) && !width && !height,
            "actual engine failure epilogue returns original false/zero outputs");
        testCaller = 0x4A5EF1;
        CHECK(measure(bitFont, objectives, &width, &height, 400) && width == 190 && height == 40,
            "failed engine call leaves no saved width in unrelated subsequent call");
        *(int*)((unsigned char*)bitFont + 0x1C) = oldHeight;
        VirtualFree(code, 0, MEM_RELEASE);
    }
}
