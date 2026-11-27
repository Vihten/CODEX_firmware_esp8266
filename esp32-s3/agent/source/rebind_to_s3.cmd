@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0rebind_to_s3.ps1" %*
set "rebind_result=%errorlevel%"
if not "%rebind_result%"=="0" (
  echo Rebind failed. See the message above.
) else (
  echo Rebind command completed. Check agent.log for a controller acknowledgement.
)
pause
exit /b %rebind_result%
