@echo off
rem ===========================================================================
rem  Render the strings the game really drew (from VectorText.log) through the
rem  real M1 takeover path: original bitmap glyphs on one row, our vector
rem  glyphs on the next, plus a glyph coverage report.
rem
rem  usage: render_strings.bat [--top N] [--min-count N] [--aa 0|1] [--out f.bmp]
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
   ..\..\tools\render_strings.cpp ..\..\src\GlyphSource.cpp ..\..\src\PixelWriter.cpp ^
   ..\..\src\Takeover.cpp ..\..\src\Logger.cpp ^
   /Fe:render_strings.exe ^
   /link ..\freetype\freetype.lib /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( popd & echo [x] build failed & exit /b 1 )

render_strings.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
