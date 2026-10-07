@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
rem Shared CMake entry: reconfigure the existing build, then build target(s).
rem Caller uses SETLOCAL. VT_CMAKE may override the detected cmake.exe.
set "VT_ROOT=%~dp0.."
rem Keep MSVC include-dependency output consistent for Ninja on localized Windows.
set "VSLANG=1033"
call "%~dp0find_vcvars.bat" || exit /b 1
rem VS 2026 defaults to a runtime that no longer targets Windows 7.
rem An older toolset installed alongside it is supported by vcvars32.
if not defined VT_VCVARS_VER set "VT_VCVARS_VER=14.44"
call "%VCVARS%" -vcvars_ver=%VT_VCVARS_VER% >nul
if errorlevel 1 (
    echo [x] Install MSVC %VT_VCVARS_VER% x86/x64 tools, or select a Windows 7 compatible version with VT_VCVARS_VER.
    exit /b 1
)
set "VT_CL="
for /f "delims=" %%I in ('where cl.exe 2^>nul') do if not defined VT_CL set "VT_CL=%%I"
if not defined VT_CL exit /b 1
if not defined VT_CMAKE for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined VT_CMAKE set "VT_CMAKE=%%I"
if not defined VT_CMAKE if exist "%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "VT_CMAKE=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined VT_CMAKE (
    echo [x] Install CMake 3.21+ and Ninja, or set VT_CMAKE to cmake.exe.
    exit /b 1
)
for %%I in ("%VT_CMAKE%") do set "PATH=%%~dpI;%PATH%"
if exist "%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "PATH=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
pushd "%VT_ROOT%"
rem Explicit compiler paths also invalidate a cache created with another toolset.
"%VT_CMAKE%" --preset x86-release "-DCMAKE_C_COMPILER=%VT_CL%" "-DCMAKE_CXX_COMPILER=%VT_CL%"
rem CMake can discard an old compiler cache after the platform guard rejects
rem it. Finish that toolset transition once, using the same preset and paths.
if errorlevel 1 if not exist "build\cmake\CMakeCache.txt" "%VT_CMAKE%" --preset x86-release "-DCMAKE_C_COMPILER=%VT_CL%" "-DCMAKE_CXX_COMPILER=%VT_CL%"
if errorlevel 1 (popd & exit /b 1)
if "%~1"=="" (
    "%VT_CMAKE%" --build --preset x86-release
) else (
    "%VT_CMAKE%" --build --preset x86-release --target %*
)
set "VT_BUILD_RC=%ERRORLEVEL%"
popd
exit /b %VT_BUILD_RC%
