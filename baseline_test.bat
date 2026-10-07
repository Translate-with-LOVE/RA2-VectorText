@echo off
setlocal
call "%~dp0tools\find_vcvars.bat"
if errorlevel 1 exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0build\baseline-test" mkdir "%~dp0build\baseline-test"
pushd "%~dp0build\baseline-test"
cl /nologo /MT /O2 /EHsc /std:c++17 /utf-8 /W4 /D WIN32_LEAN_AND_MEAN /D _CRT_SECURE_NO_WARNINGS ^
 /I "..\..\src" /I "..\..\third_party\freetype\include" ^
 "..\..\tools\baseline_test.cpp" "..\..\src\GlyphSource.cpp" "..\..\src\PixelWriter.cpp" ^
 /Fe:baseline_test.exe /link "..\freetype\freetype.lib" /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 (popd & exit /b 1)
baseline_test.exe
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
