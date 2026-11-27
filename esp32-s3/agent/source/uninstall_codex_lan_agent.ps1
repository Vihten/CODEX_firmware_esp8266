$ErrorActionPreference = 'Stop'
$installDirectory = Join-Path $env:LOCALAPPDATA 'LED Forge\Codex LAN Agent'
$installedAgent = Join-Path $installDirectory 'codex_lan_agent.ps1'
$installedSupervisor = Join-Path $installDirectory 'start_agent_supervisor.ps1'
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Startup')) 'LED Forge Codex LAN Agent.lnk'

. (Join-Path $PSScriptRoot 'agent_update.ps1')
# Remove recovery first so it cannot race the process shutdown and uninstall.
Remove-AgentAutostart $installDirectory

Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object {
        $_.ProcessId -ne $PID -and
        ($_.CommandLine -like "*$installedAgent*" -or $_.CommandLine -like "*$installedSupervisor*")
    } |
    ForEach-Object { [void](Invoke-CimMethod -InputObject $_ -MethodName Terminate) }

Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $installDirectory -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'LED Forge Codex LAN Agent removed.'
