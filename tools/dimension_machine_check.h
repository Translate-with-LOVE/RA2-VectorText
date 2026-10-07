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

    static void Jump(unsigned char* at, const void* target)
    {
        at[0] = 0xE9;
        *(DWORD*)(at + 1) = (DWORD)target - (DWORD)at - 5;
    }

    static void Stub(unsigned char* at, unsigned char* resume, const unsigned char* replay,
                     DWORD origin, void(__cdecl* dispatch)(REGISTERS*))
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
        memcpy(at, replay, 6); at += 6;
        Jump(at, resume);
    }

    static unsigned char* LoadRoutine()
    {
        FILE* file = fopen("F:\\Mental Omega\\gamemd.exe", "rb");
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
        testParent = 0x5D4706;
        CHECK(measure(bitFont, message, &width, &height, 0) && width == LegacyWidth(message),
            "actual engine body keeps original wrapping-probe width");
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
