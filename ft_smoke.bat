@echo off
rem ===========================================================================
rem  Build and run the FreeType smoke test (32-bit, statically linked).
rem  Proves: the lib links, Noto Serif SC loads, mono + coverage rasterisation
rem  work, and shows the metrics M1 will use.
rem
rem  usage: ft_smoke.bat [--face <ttf>] [--size N] [--chars "<text>"]
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
if not exist "%ROOT%build" mkdir "%ROOT%build"

pushd "%ROOT%build"
cl /nologo /MT /O2 /EHsc /W4 /D _CRT_SECURE_NO_WARNINGS ^
   /I "%ROOT%third_party\freetype\include" ^
   ..\tools\ft_smoke.cpp ^
   /Fe:ft_smoke.exe /Fo:ft_smoke.obj ^
   /link freetype\freetype.lib /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( popd & echo [x] smoke test build failed & exit /b 1 )

ft_smoke.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
