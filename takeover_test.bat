@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
setlocal
set "ROOT=%~dp0"
call "%ROOT%tools\cmake_build.bat" takeover_test || exit /b 1
pushd "%ROOT%build\test"
takeover_test.exe %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
