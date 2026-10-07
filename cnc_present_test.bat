@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
setlocal
set "ROOT=%~dp0"
call "%ROOT%tools\cmake_build.bat" cnc_present_test || exit /b 1
pushd "%ROOT%build\cnc-test"
copy /y "%ROOT%..\ddraw.dll" ddraw.dll >nul
if errorlevel 1 (popd & exit /b 1)
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
if "%~1"=="--2x-only" goto output2
if "%~1"=="--perf-only" goto performance
if "%~1"=="--fractional-only" goto fractional
for %%R in (direct3d9 opengl gdi) do (
    >ddraw.ini echo [ddraw]
    >>ddraw.ini echo renderer=%%R
    >>ddraw.ini echo windowed=true
    >>ddraw.ini echo fullscreen=false
    >>ddraw.ini echo width=640
    >>ddraw.ini echo height=480
    >>ddraw.ini echo minfps=30
    >>ddraw.ini echo singlecpu=false
    >>ddraw.ini echo hook=4
    >>ddraw.ini echo maxfps=60
    >>ddraw.ini echo d3d9_filter=0
    echo [*] cnc-ddraw backend: %%R
    cnc_present_test.exe
    if errorlevel 1 (popd & exit /b 1)
)
:output2
call :configure2
echo [*] cnc-ddraw 2x output, bicubic world + independent text
cnc_present_test.exe --2x
if errorlevel 1 (popd & exit /b 1)
rem Resize persists per-game dimensions even in this local cnc-ddraw build.
rem Start the compatibility test with a fresh effective 2x configuration.
call :configure2
>VectorText.ini echo [VectorText]
>>VectorText.ini echo Enabled=true
>>VectorText.ini echo Mode=draw
>>VectorText.ini echo Present32=true
>>VectorText.ini echo HiDPI=false
cnc_present_test.exe --compat-2x
if errorlevel 1 (copy /y "%ROOT%VectorText.ini" VectorText.ini >nul & popd & exit /b 1)
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
call :fractional_checks
if errorlevel 1 (popd & exit /b 1)
for %%W in (true false) do (
    >ddraw.ini echo [ddraw]
    >>ddraw.ini echo renderer=direct3d9
    >>ddraw.ini echo windowed=%%W
    >>ddraw.ini echo fullscreen=true
    >>ddraw.ini echo border=false
    >>ddraw.ini echo savesettings=0
    >>ddraw.ini echo minfps=30
    >>ddraw.ini echo singlecpu=false
    >>ddraw.ini echo hook=4
    >>ddraw.ini echo maxfps=60
    echo [*] cnc-ddraw native fullscreen, windowed=%%W
    cnc_present_test.exe --fullscreen
    if errorlevel 1 (popd & exit /b 1)
)
>ddraw.ini echo [ddraw]
>>ddraw.ini echo renderer=direct3d9
>>ddraw.ini echo windowed=true
>>ddraw.ini echo fullscreen=false
>>ddraw.ini echo width=640
>>ddraw.ini echo height=480
>>ddraw.ini echo minfps=30
>>ddraw.ini echo singlecpu=false
>>ddraw.ini echo hook=4
cnc_present_test.exe --no-cnc
if errorlevel 1 (popd & exit /b 1)
>VectorText.ini echo [VectorText]
>>VectorText.ini echo Enabled=true
>>VectorText.ini echo Mode=draw
>>VectorText.ini echo Present32=false
cnc_present_test.exe --fallback
set "RC=%ERRORLEVEL%"
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
popd
exit /b %RC%

:fractional
call :fractional_checks
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%

:fractional_checks
for %%S in (1.25 1.5 1.75 2.25 2.5) do (
    call :configure_fraction %%S
    echo [*] cnc-ddraw fractional output %%S
    cnc_present_test.exe --fractional %%S
    if errorlevel 1 exit /b 1
)
call :configure_fraction 1.5
>VectorText.ini echo [VectorText]
>>VectorText.ini echo Enabled=true
>>VectorText.ini echo Mode=draw
>>VectorText.ini echo Present32=true
>>VectorText.ini echo HiDPI=false
cnc_present_test.exe --fractional 1.5
set "VT_FRACTION_RC=%ERRORLEVEL%"
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
exit /b %VT_FRACTION_RC%

:configure_fraction
set "VT_TEST_WIDTH=960"
set "VT_TEST_HEIGHT=720"
if "%~1"=="1.25" (set "VT_TEST_WIDTH=800" & set "VT_TEST_HEIGHT=600")
if "%~1"=="1.75" (set "VT_TEST_WIDTH=1120" & set "VT_TEST_HEIGHT=840")
if "%~1"=="2.25" (set "VT_TEST_WIDTH=1440" & set "VT_TEST_HEIGHT=1080")
if "%~1"=="2.5" (set "VT_TEST_WIDTH=1600" & set "VT_TEST_HEIGHT=1200")
call :configure2 %VT_TEST_WIDTH% %VT_TEST_HEIGHT%
exit /b 0

:performance
>ddraw.ini echo [ddraw]
>>ddraw.ini echo renderer=direct3d9
>>ddraw.ini echo windowed=true
>>ddraw.ini echo fullscreen=true
>>ddraw.ini echo border=false
>>ddraw.ini echo savesettings=0
>>ddraw.ini echo singlecpu=true
>>ddraw.ini echo minfps=30
>>ddraw.ini echo maxfps=60
>>ddraw.ini echo hook=4
>>ddraw.ini echo d3d9_filter=2
cnc_present_test.exe --perf-2x
set "RC=%ERRORLEVEL%"
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
popd
exit /b %RC%

:configure2
set "VT_OUTPUT_WIDTH=1280"
set "VT_OUTPUT_HEIGHT=960"
if not "%~1"=="" set "VT_OUTPUT_WIDTH=%~1"
if not "%~2"=="" set "VT_OUTPUT_HEIGHT=%~2"
>ddraw.ini echo [ddraw]
>>ddraw.ini echo renderer=direct3d9
>>ddraw.ini echo windowed=true
>>ddraw.ini echo fullscreen=false
>>ddraw.ini echo width=%VT_OUTPUT_WIDTH%
>>ddraw.ini echo height=%VT_OUTPUT_HEIGHT%
>>ddraw.ini echo resizable=true
>>ddraw.ini echo savesettings=0
>>ddraw.ini echo minfps=30
>>ddraw.ini echo singlecpu=false
>>ddraw.ini echo hook=4
>>ddraw.ini echo maxfps=60
>>ddraw.ini echo d3d9_filter=2
exit /b 0
