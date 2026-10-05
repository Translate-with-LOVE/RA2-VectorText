@echo off
rem ===========================================================================
rem  Launch the game through Syringe so VectorText.dll is loaded.
rem
rem  The command line below is the one the Mental Omega client uses for its own
rem  launches (taken from syringe.log).  This script only exists so the hook DLL
rem  can be tested without the full client; you can equally just start the game
rem  through the normal MO launcher.
rem
rem  Play for a while, exit normally, then read VectorText.log next to gamemd.exe.
rem ===========================================================================
cd /d "%~dp0.."

if not exist Syringe.exe (
  echo [x] Syringe.exe not found in "%CD%"
  exit /b 1
)
if not exist VectorText.dll (
  echo [x] VectorText.dll not built yet -- run VectorText\build.bat first
  exit /b 1
)

echo [*] working directory: %CD%
echo [*] launching: Syringe.exe "gamemd.exe -SPAWN -CD -SPEEDCONTROL -LOG -AFFINITY:4095"

rem Alternative (plain skirmish launch, no spawn mode):
rem   Syringe.exe "gamemd.exe -CD -LOG"
Syringe.exe "gamemd.exe -SPAWN -CD -SPEEDCONTROL -LOG -AFFINITY:4095"

echo [*] game exited. Log: "%CD%\VectorText.log"
