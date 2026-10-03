@echo off
rem Starts the CyberCraft Minecraft client (dev launcher, offline name "Dovahkiin").
rem It waits on the title screen until Cyberpunk 2077 (with the CyberCraft plugin) is running,
rem then hides its window and loads the mirror world by itself.
cd /d "%~dp0..\fabric"
call gradlew.bat runClient --no-configuration-cache
