@echo off
rem ===========================================================================
rem  Build and run the pixel writer test (M1.1 formula, offline).
rem  Renders game.fnt glyphs and our vector glyphs into a synthetic 16-bit
rem  surface with the engine's addressing/colour rules, dumps ASCII art and
rem  writes build\test\pixel_test.bmp
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
   /I "%ROOT%third_party\freetype\include" ^
   ..\..\tools\pixel_test.cpp ..\..\src\GlyphSource.cpp ..\..\src\PixelWriter.cpp ^
   /Fe:pixel_test.exe ^
   /link ..\freetype\freetype.lib /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( popd & echo [x] pixel test build failed & exit /b 1 )

pixel_test.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
