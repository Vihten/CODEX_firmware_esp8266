@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0configure_peer_firewall.ps1" -Action Enable
if errorlevel 1 (
  echo Open an Administrator terminal and rerun this command to enable the rules.
  exit /b 1
)
echo Direct peer chat firewall rules enabled.
