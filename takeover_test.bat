@echo off
rem ===========================================================================
rem  Build and run the takeover test: feeds a synthetic BitFont object (same
rem  layout the engine uses) with the real game.fnt tables and checks the M1
rem  takeover path end to end, without starting the game.
rem
rem  usage: takeover_test.bat [--aa 0|1] [--out file.bmp]
rem ===========================================================================
setlocal
set "ROOT=%~dp0"

call "%ROOT%tools\find_vcvars.bat"
if errorlevel 1 exit /b 1
call "%VCVARS%" >nul || (echo [x] vcvars32.bat failed & exit /b 1)

if not exist "%ROOT%build\freetype\freetype.lib" (
  echo [*] building FreeType first...
  call "%ROOT%third_party\build_freetype.bat" || exit /b 1
)
if not exist "%ROOT%build\test" mkdir "%ROOT%build\test"

pushd "%ROOT%build\test"
cl /nologo /MT /O2 /EHsc /W4 /D _CRT_SECURE_NO_WARNINGS ^
   /I "%ROOT%third_party\freetype\include" /I "%ROOT%src" /I "%ROOT%include" ^
   ..\..\tools\takeover_test.cpp ..\..\src\GlyphSource.cpp ..\..\src\PixelWriter.cpp ^
   ..\..\src\Takeover.cpp ..\..\src\Logger.cpp ^
   /Fe:takeover_test.exe ^
   /link ..\freetype\freetype.lib /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( popd & echo [x] takeover test build failed & exit /b 1 )

takeover_test.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
