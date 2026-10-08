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
if "%~1"=="--stretch-only" goto stretched
if "%~1"=="--parity-only" goto parity
if "%~1"=="--startup-only" goto startup
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
if "%~1"=="--2x-only" goto output2_legacy
call :parity_checks
if errorlevel 1 (popd & exit /b 1)
:output2_legacy
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
call :stretch_checks
if errorlevel 1 exit /b 1
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

:stretched
call :stretch_checks
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%

:parity
call :parity_checks
set "RC=%ERRORLEVEL%"
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
popd
exit /b %RC%

:startup
for %%R in (direct3d9 opengl gdi) do (
    copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
    call :configure2 3840 2160 %%R
    echo [*] %%R: one-shot 4K startup, no repaint or second upload
    cnc_present_test.exe --startup 6 4.5
    if errorlevel 1 (popd & exit /b 1)
)
popd
exit /b 0

:parity_checks
for %%R in (direct3d9 opengl gdi) do (
    copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
    call :configure2 1280 960 %%R
    echo [*] %%R: exact 2x samples, partial alpha and atlas lifecycle
    cnc_present_test.exe --2x
    if errorlevel 1 exit /b 1
    call :configure2 3072 1728 %%R
    echo [*] %%R: 4.8x3.6, subtitles and returning captions
    cnc_present_test.exe --fractional 4.8 3.6
    if errorlevel 1 exit /b 1
    call :configure2 960 720 %%R
    echo [*] %%R: 1.5x and live viewport changes
    cnc_present_test.exe --fractional 1.5
    if errorlevel 1 exit /b 1
    call :configure2 3072 1728 %%R
    >VectorText.ini echo [VectorText]
    >>VectorText.ini echo Enabled=true
    >>VectorText.ini echo Mode=draw
    >>VectorText.ini echo Present32=true
    >>VectorText.ini echo HiDPI=true
    >>VectorText.ini echo LinearBlend=false
    echo [*] %%R: non-linear blending
    cnc_present_test.exe --fractional 4.8 3.6
    if errorlevel 1 exit /b 1
    call :configure2 1536 864 %%R
    >VectorText.ini echo [VectorText]
    >>VectorText.ini echo Enabled=true
    >>VectorText.ini echo Mode=draw
    >>VectorText.ini echo Present32=true
    >>VectorText.ini echo HiDPI=false
    echo [*] %%R: HiDPI disabled
    cnc_present_test.exe --fractional 2.4 1.8
    if errorlevel 1 exit /b 1
)
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
exit /b 0

:stretch_checks
call :configure2 3072 1728
echo [*] cnc-ddraw original startup scale 4.8x3.6 with independent 5x glyphs
cnc_present_test.exe --fractional 4.8 3.6
if errorlevel 1 exit /b 1
call :configure2 1536 864
echo [*] cnc-ddraw widescreen output 2.4x1.8
cnc_present_test.exe --fractional 2.4 1.8
if errorlevel 1 exit /b 1
call :configure2 1536 864
>VectorText.ini echo [VectorText]
>>VectorText.ini echo Enabled=true
>>VectorText.ini echo Mode=draw
>>VectorText.ini echo Present32=true
>>VectorText.ini echo HiDPI=false
cnc_present_test.exe --fractional 2.4 1.8
set "VT_STRETCH_RC=%ERRORLEVEL%"
copy /y "%ROOT%VectorText.ini" VectorText.ini >nul
exit /b %VT_STRETCH_RC%

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
set "VT_TEST_RENDERER=direct3d9"
if not "%~3"=="" set "VT_TEST_RENDERER=%~3"
set "VT_OUTPUT_WIDTH=1280"
set "VT_OUTPUT_HEIGHT=960"
if not "%~1"=="" set "VT_OUTPUT_WIDTH=%~1"
if not "%~2"=="" set "VT_OUTPUT_HEIGHT=%~2"
>ddraw.ini echo [ddraw]
>>ddraw.ini echo renderer=%VT_TEST_RENDERER%
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
