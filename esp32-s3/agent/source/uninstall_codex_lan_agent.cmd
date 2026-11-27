@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall_codex_lan_agent.ps1"
if errorlevel 1 pause
