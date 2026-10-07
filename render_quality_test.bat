@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
setlocal
set "ROOT=%~dp0"
call "%ROOT%tools\cmake_build.bat" render_quality || exit /b 1
pushd "%ROOT%build\render-qa"
copy /y "..\..\VectorText.ini" "VectorText.ini" >nul
render_quality.exe raster-after.bmp --check --line
if errorlevel 1 (popd & exit /b 1)
render_quality.exe raster-scene.bmp --check --line --scene
if errorlevel 1 (popd & exit /b 1)
render_quality.exe bgra-after.bmp --check --line --bgra
if errorlevel 1 (popd & exit /b 1)
render_quality.exe bgra-scene.bmp --check --line --scene --bgra
if errorlevel 1 (popd & exit /b 1)
render_quality.exe bgra-guides.bmp --check --line --bgra --guides
if errorlevel 1 (popd & exit /b 1)
render_quality.exe loading-2x.bmp --check --line --bgra --loading --hidpi
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
