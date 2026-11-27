@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_codex_lan_agent.ps1" %*
if errorlevel 1 pause
