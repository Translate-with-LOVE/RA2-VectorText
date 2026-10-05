@echo off
rem ===========================================================================
rem  VectorText - build script  (32-bit DLL, static CRT, no dependencies)
rem
rem  Output: <game dir>\VectorText.dll   (next to gamemd.exe, where Syringe
rem          scans for hook DLLs) and <game dir>\VectorText.ini
rem
rem  Requires any MSVC with the x86 tools (Visual Studio Build Tools are enough).
rem  If detection fails, set VCVARS manually below.
rem ===========================================================================
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
set "GAMEDIR=%~dp0.."
set "OUT=%GAMEDIR%\VectorText.dll"

call "%ROOT%tools\find_vcvars.bat"
if errorlevel 1 exit /b 1

echo [*] toolchain: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (
  echo [x] vcvars32.bat failed
  exit /b 1
)

if not exist "%ROOT%build" mkdir "%ROOT%build"

rem FreeType is linked statically into the DLL (TrueType + autohinter + mono/smooth rasterisers)
if not exist "%ROOT%build\freetype\freetype.lib" (
  echo [*] building FreeType first time...
  call "%ROOT%third_party\build_freetype.bat"
  if errorlevel 1 exit /b 1
)

pushd "%ROOT%build"

cl /nologo /LD /MT /O2 /EHsc /std:c++17 /W4 ^
   /D SYR_VER=2 /D WIN32_LEAN_AND_MEAN /D _CRT_SECURE_NO_WARNINGS ^
   /I "..\include" /I "..\src" /I "..\third_party\freetype\include" ^
   ..\src\Logger.cpp ..\src\Hooks.cpp ..\src\DllMain.cpp ^
   ..\src\GlyphSource.cpp ..\src\PixelWriter.cpp ..\src\Takeover.cpp ^
   /Fe:"%OUT%" /Fd:"VectorText.pdb" ^
   /link /SUBSYSTEM:WINDOWS /MACHINE:X86 /INCREMENTAL:NO ^
   freetype\freetype.lib ^
   /IMPLIB:"VectorText.lib" /OUT:"%OUT%"

set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
  echo [x] build failed with %RC%
  exit /b %RC%
)

copy /y "%ROOT%VectorText.ini" "%GAMEDIR%\VectorText.ini" >nul

echo [ok] %OUT%
echo [ok] %GAMEDIR%\VectorText.ini
echo.
echo Next: launch the game through Syringe (normally the MO launcher does this),
echo       play for a while, then inspect %GAMEDIR%\VectorText.log
