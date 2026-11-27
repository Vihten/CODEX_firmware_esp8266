@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_hub.ps1"
if errorlevel 1 pause
