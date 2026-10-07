@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
setlocal
call "%~dp0tools\cmake_build.bat" deploy
exit /b %ERRORLEVEL%
