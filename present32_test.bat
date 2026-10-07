@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
setlocal
set "ROOT=%~dp0"
call "%ROOT%tools\cmake_build.bat" present32_test || exit /b 1
pushd "%ROOT%build\present32-test"
present32_test.exe
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
