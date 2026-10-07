@echo off
rem SPDX-FileCopyrightText: 2026 VectorText contributors
rem SPDX-License-Identifier: GPL-3.0-only
rem ===========================================================================
rem  locate vcvars32.bat and export VCVARS to the CALLER's environment.
rem  Usage (from another script, inside its setlocal):
rem      call "%~dp0tools\find_vcvars.bat" || exit /b 1
rem ===========================================================================

if defined VT_VCVARS if exist "%VT_VCVARS%" set "VCVARS=%VT_VCVARS%"

if not defined VCVARS (
  for %%v in (18 17 16 15) do (
    if not defined VCVARS (
      for %%e in (BuildTools Community Professional Enterprise) do (
        if not defined VCVARS (
          if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars32.bat" (
            set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars32.bat"
          )
        )
      )
    )
  )
)

if not defined VCVARS (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
      if exist "%%i\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
    )
  )
)

if not defined VCVARS (
  echo [x] vcvars32.bat not found.
  echo     Install "Desktop development with C++" or set VT_VCVARS to its full path.
  exit /b 1
)
exit /b 0
