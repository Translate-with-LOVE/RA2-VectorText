@echo off
setlocal
call "%~dp0tools\find_vcvars.bat"
if errorlevel 1 exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0build\line-test" mkdir "%~dp0build\line-test"
pushd "%~dp0build\line-test"
cl /nologo /MT /O2 /EHsc /std:c++17 /utf-8 /W4 /D WIN32_LEAN_AND_MEAN /D _CRT_SECURE_NO_WARNINGS ^
 /I "..\..\include" /I "..\..\src" /I "..\..\third_party\freetype\include" ^
 "..\..\tools\line_render_test.cpp" "..\..\src\GlyphSource.cpp" "..\..\src\PixelWriter.cpp" ^
 "..\..\src\Takeover.cpp" "..\..\src\Logger.cpp" /Fe:line_render_test.exe ^
 /link "..\freetype\freetype.lib" /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 (popd & exit /b 1)
copy /y "..\..\..\VectorText.dll" "VectorText.dll" >nul
copy /y "..\..\VectorText.ini" "VectorText.ini" >nul
line_render_test.exe
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
