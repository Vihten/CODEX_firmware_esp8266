@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0configure_hub_firewall.ps1" %*
if errorlevel 1 pause
