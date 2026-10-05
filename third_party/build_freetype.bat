@echo off
rem ===========================================================================
rem  Build a static, 32-bit FreeType library for VectorText.dll.
rem
rem  Source : third_party\freetype  (git submodule, pinned to a release tag)
rem  Output : build\freetype\freetype.lib   +  build\freetype\include marker
rem
rem  This is FreeType's documented "single object" build: a handful of .c files
rem  each of which #includes the modules it needs.  No CFF/Type1/bzip2/png/gzip
rem  support -- TrueType (glyf) with the auto-hinter, mono + smooth rasterisers
rem  is all M1/M2 need.
rem ===========================================================================
setlocal
set "ROOT=%~dp0.."
call "%ROOT%\tools\find_vcvars.bat"
if errorlevel 1 exit /b 1
call "%VCVARS%" >nul || (echo [x] vcvars32.bat failed & exit /b 1)

set "FT=%ROOT%\third_party\freetype"
set "OUT=%ROOT%\build\freetype"

if not exist "%FT%\include\ft2build.h" (
  echo [x] FreeType source missing at "%FT%"
  echo     run:  git submodule update --init --recursive
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

echo [*] FreeType source : %FT%
pushd "%OUT%"

cl /nologo /c /MT /O2 /W3 /wd4819 /D FT2_BUILD_LIBRARY /D _CRT_SECURE_NO_WARNINGS ^
   /DFT_CONFIG_MODULES_H=\"ftmodule.min.h\" ^
   /I "%FT%\include" /I "%ROOT%\third_party" ^
   "%FT%\src\base\ftsystem.c" ^
   "%FT%\src\base\ftinit.c" ^
   "%FT%\src\base\ftdebug.c" ^
   "%FT%\src\base\ftbase.c" ^
   "%FT%\src\base\ftbitmap.c" ^
   "%FT%\src\base\ftmm.c" ^
   "%FT%\src\base\ftgasp.c" ^
   "%FT%\src\gzip\ftgzip.c" ^
   "%FT%\src\truetype\truetype.c" ^
   "%FT%\src\sfnt\sfnt.c" ^
   "%FT%\src\smooth\smooth.c" ^
   "%FT%\src\raster\raster.c" ^
   "%FT%\src\autofit\autofit.c" ^
   "%FT%\src\psnames\psmodule.c"
if errorlevel 1 ( popd & echo [x] compile failed & exit /b 1 )

lib /nologo /out:freetype.lib *.obj
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" ( echo [x] lib failed & exit /b 1 )

echo [ok] %OUT%\freetype.lib
dir /b "%OUT%\*.obj" | find /c /v "" > nul
