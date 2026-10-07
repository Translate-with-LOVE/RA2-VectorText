@echo off
setlocal
call "%~dp0tools\find_vcvars.bat"
if errorlevel 1 exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0build\freetype\freetype.lib" (
  call "%~dp0third_party\build_freetype.bat"
  if errorlevel 1 exit /b 1
)
if not exist "%~dp0build\render-qa" mkdir "%~dp0build\render-qa"
pushd "%~dp0build\render-qa"
cl /nologo /MT /O2 /EHsc /std:c++17 /utf-8 /W4 /D WIN32_LEAN_AND_MEAN /D _CRT_SECURE_NO_WARNINGS ^
 /I "..\..\include" /I "..\..\src" /I "..\..\third_party\freetype\include" ^
 "..\..\tools\render_quality_test.cpp" "..\..\src\GlyphSource.cpp" "..\..\src\PixelWriter.cpp" ^
 "..\..\src\Takeover.cpp" "..\..\src\Logger.cpp" /Fe:render_quality.exe ^
 /link "..\freetype\freetype.lib" /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 (popd & exit /b 1)
copy /y "..\..\VectorText.ini" "VectorText.ini" >nul
render_quality.exe raster-after.bmp --check --line
if errorlevel 1 (popd & exit /b 1)
render_quality.exe raster-scene.bmp --check --line --scene
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
