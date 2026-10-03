@echo off
rem Builds CyberCraft and installs it into Cyberpunk 2077, replacing the version already there
rem (one Vortex installed included). Close the game first. Arguments go to tools\install.ps1:
rem   -NoBuild                         install what was built last
rem   -Game "D:\Games\Cyberpunk 2077"   if it is not found by itself
rem   -NoVortex                        leave Vortex's copy alone
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install.ps1" %*
pause
