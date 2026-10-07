@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
rem Shared CMake entry: configure once/incrementally, then build target(s).
rem Caller uses SETLOCAL. VT_CMAKE may override the detected cmake.exe.
set "VT_ROOT=%~dp0.."
rem Keep MSVC include-dependency output consistent for Ninja on localized Windows.
set "VSLANG=1033"
call "%~dp0find_vcvars.bat" || exit /b 1
call "%VCVARS%" >nul || exit /b 1
if not defined VT_CMAKE for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined VT_CMAKE set "VT_CMAKE=%%I"
if not defined VT_CMAKE if exist "%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "VT_CMAKE=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not defined VT_CMAKE (
    echo [x] Install CMake 3.21+ and Ninja, or set VT_CMAKE to cmake.exe.
    exit /b 1
)
for %%I in ("%VT_CMAKE%") do set "PATH=%%~dpI;%PATH%"
if exist "%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "PATH=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
pushd "%VT_ROOT%"
"%VT_CMAKE%" --preset x86-release
if errorlevel 1 (popd & exit /b 1)
if "%~1"=="" (
    "%VT_CMAKE%" --build --preset x86-release
) else (
    "%VT_CMAKE%" --build --preset x86-release --target %*
)
set "VT_BUILD_RC=%ERRORLEVEL%"
popd
exit /b %VT_BUILD_RC%
