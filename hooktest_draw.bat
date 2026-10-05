@echo off
rem ===========================================================================
rem  Hook-glue test, in draw mode: drives the exported BitFont::Blit hook with a
rem  synthetic REGISTERS block (exactly how Syringe calls it) and checks the
rem  skip-the-callee contract: return address, ESP popped by 0x14, EAX = pen X,
rem  and that the glyph really landed in the locked surface.  Also checks that
rem  every refusal path returns 0 with ESP untouched (byte-exact fallback).
rem
rem  usage: hooktest_draw.bat [--mode draw|observe]
rem ===========================================================================
setlocal
set "ROOT=%~dp0"

call "%ROOT%tools\find_vcvars.bat"
if errorlevel 1 exit /b 1
call "%VCVARS%" >nul || (echo [x] vcvars32.bat failed & exit /b 1)

if not exist "%ROOT%..\VectorText.dll" (
  echo [x] VectorText.dll not built yet -- run build.bat first
  exit /b 1
)
if not exist "%ROOT%build\test" mkdir "%ROOT%build\test"

pushd "%ROOT%build\test"
cl /nologo /MT /O2 /EHsc /W4 /D _CRT_SECURE_NO_WARNINGS ^
   ..\..\tools\hooktest_draw.cpp /Fe:hooktest_draw.exe /link /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( popd & echo [x] build failed & exit /b 1 )

copy /y "%ROOT%..\VectorText.dll" "VectorText.dll" >nul
del /q VectorText.log >nul 2>nul
echo.
hooktest_draw.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
