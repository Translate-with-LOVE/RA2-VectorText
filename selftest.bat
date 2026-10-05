@echo off
rem ===========================================================================
rem  VectorText - offline self test.
rem
rem  Loads the built VectorText.dll into a small console process, calls every
rem  hook handler with a synthetic REGISTERS block and checks that
rem    * all hook exports exist and return 0 (pass-through),
rem    * the logger initialises in the calling process and writes its log.
rem
rem  It does NOT start the game and does NOT touch the game directory.
rem  Run build.bat first.
rem ===========================================================================
setlocal
set "ROOT=%~dp0"

call "%ROOT%tools\find_vcvars.bat"
if errorlevel 1 exit /b 1
echo [*] toolchain: %VCVARS%
call "%VCVARS%" >nul || (echo [x] vcvars32.bat failed & exit /b 1)

if not exist "%ROOT%..\VectorText.dll" (
  echo [x] VectorText.dll not built yet -- run build.bat first
  exit /b 1
)
if not exist "%ROOT%build" mkdir "%ROOT%build"

pushd "%ROOT%build"
cl /nologo /MT /O2 /EHsc /W4 /D _CRT_SECURE_NO_WARNINGS ^
   ..\tools\selftest.cpp /Fe:selftest.exe /Fo:selftest.obj /link /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 (
  popd
  echo [x] selftest build failed
  exit /b 1
)

rem run it next to a copy of the DLL so the log lands in .\build
copy /y "..\..\VectorText.dll" "VectorText.dll" >nul
del /q VectorText.log >nul 2>nul
echo.
selftest.exe
set "RC=%ERRORLEVEL%"
popd

echo.
if "%RC%"=="0" (echo [ok] self test passed) else (echo [x] self test FAILED ^(%RC%^))
exit /b %RC%
