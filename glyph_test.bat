@echo off
rem ===========================================================================
rem  Build and run the glyph alignment test:
rem    game.fnt's own 1bpp cells  vs  our FreeType cells (engine format)
rem  Run from anywhere; paths are resolved relative to this script.
rem
rem  usage: glyph_test.bat [--size N] [--wght N] [--baseline N] [--chars "..."]
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

if not exist "%ROOT%build\test" mkdir "%ROOT%build\test"

pushd "%ROOT%build\test"
cl /nologo /MT /O2 /EHsc /W4 /D _CRT_SECURE_NO_WARNINGS ^
   /I "%ROOT%third_party\freetype\include" ^
   ..\..\tools\glyph_test.cpp ..\..\src\GlyphSource.cpp ^
   /Fe:glyph_test.exe ^
   /link ..\freetype\freetype.lib /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( popd & echo [x] glyph test build failed & exit /b 1 )

glyph_test.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
