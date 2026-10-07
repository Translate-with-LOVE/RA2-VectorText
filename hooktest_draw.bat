@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
setlocal
set "ROOT=%~dp0"
call "%ROOT%tools\cmake_build.bat" hooktest_draw VectorText || exit /b 1
pushd "%ROOT%build\test"
copy /y "%ROOT%build\cmake\bin\VectorText.dll" "VectorText.dll" >nul
del /q VectorText.log >nul 2>nul
echo.
hooktest_draw.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
