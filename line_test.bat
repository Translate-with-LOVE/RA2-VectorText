@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
setlocal
set "ROOT=%~dp0"
call "%ROOT%tools\cmake_build.bat" line_render_test VectorText || exit /b 1
pushd "%ROOT%build\line-test"
copy /y "%ROOT%build\cmake\bin\VectorText.dll" "VectorText.dll" >nul
copy /y "..\..\VectorText.ini" "VectorText.ini" >nul
line_render_test.exe
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (popd & exit /b %RC%)
for %%M in (true false numeric missing invalid) do (
    line_render_test.exe --config-encoding %%M
    if errorlevel 1 (copy /y "..\..\VectorText.ini" "VectorText.ini" >nul & popd & exit /b 1)
)
copy /y "..\..\VectorText.ini" "VectorText.ini" >nul
popd
exit /b %RC%
