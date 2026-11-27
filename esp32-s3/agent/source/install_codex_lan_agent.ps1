[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$CollectorHost = '10.0.0.97',
    [ValidateRange(1, 65535)]
    [int]$Port = 4212,
    [string]$DirectHost = '10.0.80.227',
    [ValidateRange(1, 65535)]
    [int]$DirectPort = 4210,
    [ValidateRange(1, 65535)]
    [int]$DirectHttpPort = 80,
    [string]$AgentName = '',
    [string]$ColorHex6 = '',
    [ValidatePattern('\A[0-9A-Fa-f]{6}\z')]
    [string]$ControllerDeviceId,
    [string]$AgentUpdatePublicKeyPath = ''
)

$ErrorActionPreference = 'Stop'
function Confirm-AgentStartup {
    param(
        [Diagnostics.Process]$Process,
        [string]$ErrorPath,
        [string]$LogPath
    )
    if (-not $Process.WaitForExit(2000)) { return }
    $detail = ''
    if (Test-Path -LiteralPath $ErrorPath) {
        $detail = Get-Content -LiteralPath $ErrorPath -Raw -Encoding UTF8
    }
    if ($Process.ExitCode -eq 0) {
        throw "Agent did not start (exit 0). An existing agent may still hold the instance mutex and could not be stopped automatically. Stop the old agent manually, then rerun the installer. No process with an unknown command line was terminated. Log: $LogPath. $detail"
    }
    throw "Agent exited during startup (code $($Process.ExitCode)). Error log: $ErrorPath. Agent log: $LogPath. $detail"
}

. (Join-Path $PSScriptRoot 'agent_update.ps1')
$codeFiles = Get-AgentCodeFiles
foreach ($file in $codeFiles) {
    $path = Join-Path $PSScriptRoot $file
    Assert-AgentPlainPath $path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing agent package file: $file. Extract the entire archive before installation."
    }
}
. (Join-Path $PSScriptRoot 'agent_chat_package.ps1')
$ipcInstallBytes = Get-AgentChatIpcInstallBytes $PSScriptRoot
. (Join-Path $PSScriptRoot 'agent_metadata.ps1')
[void](Get-AgentInstalledVersion $PSScriptRoot)
if (-not $AgentUpdatePublicKeyPath) {
    $candidates = @((Join-Path $PSScriptRoot 'agent-update-public.xml'), (Join-Path $PSScriptRoot '../../build/agent-release/agent-update-public.xml'))
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $AgentUpdatePublicKeyPath = $candidate; break }
    }
}
if ($AgentUpdatePublicKeyPath) {
    Assert-AgentPlainPath $AgentUpdatePublicKeyPath
    $publicXml = [IO.File]::ReadAllText($AgentUpdatePublicKeyPath)
    $rsa = [Security.Cryptography.RSACryptoServiceProvider]::new()
    $rsa.PersistKeyInCsp = $false
    try {
        $rsa.FromXmlString($publicXml)
        if (-not $rsa.PublicOnly -or $rsa.KeySize -lt 2048) { throw 'Expected RSA public key only' }
    } finally { $rsa.Dispose() }
}
$installDirectory = Join-Path $env:LOCALAPPDATA 'LED Forge\Codex LAN Agent'
$metadataPath = Join-Path $installDirectory 'agent-metadata.json'
$savedMetadata = [pscustomobject]@{}
if (Test-Path -LiteralPath $metadataPath) {
    $savedMetadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $PSBoundParameters.ContainsKey('AgentName')) { $AgentName = $savedMetadata.Name }
    if (-not $PSBoundParameters.ContainsKey('ColorHex6')) { $ColorHex6 = $savedMetadata.ColorHex6 }
}
$defaults = Get-AgentMetadata
if (-not $PSBoundParameters.ContainsKey('AgentName') -and [string]::IsNullOrWhiteSpace($AgentName) -and -not $WhatIfPreference) {
    $AgentName = Read-Host "Agent name (Enter for $($defaults.Name))"
}
$metadata = Get-AgentMetadata -Name $AgentName -ColorHex6 $ColorHex6
$rebindState = $null
if ($PSBoundParameters.ContainsKey('ControllerDeviceId')) {
    if (-not $PSBoundParameters.ContainsKey('DirectHost')) { throw 'Explicit controller selection requires -DirectHost.' }
    . (Join-Path $PSScriptRoot 'controller_rebind.ps1')
    $existingEndpointPath = Join-Path $installDirectory 'controller-endpoint.json'
    if (Test-Path -LiteralPath $existingEndpointPath) {
        $existingEndpoint = Get-Content -LiteralPath $existingEndpointPath -Raw | ConvertFrom-Json
        if ($existingEndpoint.Account -ne '53197668') { throw 'Existing controller account differs from this agent; refusing to replace it.' }
    }
    $rebindState = New-ControllerRebindState -DirectHost $DirectHost -DeviceId $ControllerDeviceId -DirectPort $DirectPort -DirectHttpPort $DirectHttpPort
}

$installedAgent = Join-Path $installDirectory 'codex_lan_agent.ps1'
$installedSupervisor = Join-Path $installDirectory 'start_agent_supervisor.ps1'
$logPath = Join-Path $installDirectory 'agent.log'
$startupDirectory = [Environment]::GetFolderPath('Startup')
$shortcutPath = Join-Path $startupDirectory 'LED Forge Codex LAN Agent.lnk'

if (-not $PSCmdlet.ShouldProcess($installDirectory, "Install/restart agent '$($metadata.Name)' (color $($metadata.ColorHex6)); controller ${DirectHost}:$DirectPort device '$ControllerDeviceId'")) { return }
New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Assert-AgentPlainPath $installDirectory

# Reject a foreign fixed-name shortcut before disabling a task or changing code.
Assert-AgentStartupShortcutOwner $installDirectory $shortcutPath
$recoveryTask = Get-AgentRecoveryTask
if ($null -ne $recoveryTask) {
    Assert-AgentRecoveryTaskOwner $installDirectory $recoveryTask
    Disable-ScheduledTask -TaskName $recoveryTask.TaskName -TaskPath '\' -ErrorAction Stop | Out-Null
}
try {

Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
    Where-Object {
        $_.ProcessId -ne $PID -and
        ($_.CommandLine -like "*$installedAgent*" -or $_.CommandLine -like "*$installedSupervisor*")
    } |
    ForEach-Object { [void](Invoke-CimMethod -InputObject $_ -MethodName Terminate) }

# An explicit reinstall restores a remotely removed agent without resetting its data.
Reset-AgentActionForReinstall $installDirectory

foreach ($file in $codeFiles) {
    if ($file -eq 'agent_chat_ipc.ps1') { Write-AgentDurable (Join-Path $installDirectory $file) $ipcInstallBytes }
    else { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $installDirectory -Force }
}
if ($AgentUpdatePublicKeyPath) {
    Write-AgentDurable (Join-Path $installDirectory 'agent-update-public.xml') ([Text.Encoding]::UTF8.GetBytes($publicXml))
} else { Write-Host 'Update public key absent: updates disabled; build a release and run manual bootstrap.' }
if (-not (Test-Path -LiteralPath $metadataPath) -or
    $PSBoundParameters.ContainsKey('AgentName') -or $PSBoundParameters.ContainsKey('ColorHex6')) {
    $savedMetadata | Add-Member -NotePropertyName Name -NotePropertyValue $metadata.Name -Force
    $savedMetadata | Add-Member -NotePropertyName ColorHex6 -NotePropertyValue $metadata.ColorHex6 -Force
    $savedMetadata | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $metadataPath -Encoding UTF8
}
if ($null -ne $rebindState) {
    $endpointPath = Join-Path $installDirectory 'controller-endpoint.json'
    if (Test-Path -LiteralPath $endpointPath) {
        $backupPath = "$endpointPath.$([guid]::NewGuid().ToString('N')).bak"
        Copy-Item -LiteralPath $endpointPath -Destination $backupPath
    }
    $rebindState | ConvertTo-Json | Set-Content -LiteralPath $endpointPath -Encoding UTF8
}

$arguments = @(
    '-NoProfile'
    '-ExecutionPolicy Bypass'
    '-WindowStyle Hidden'
    "-File `"$installedSupervisor`""
    "-CollectorHost `"$CollectorHost`""
    "-Port $Port"
    "-DirectHost `"$DirectHost`""
    "-DirectPort $DirectPort"
    "-DirectHttpPort $DirectHttpPort"
    "-LogPath `"$logPath`""
) -join ' '

Write-AgentGuiLauncher $installDirectory $arguments
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = "$env:SystemRoot\System32\wscript.exe"
$shortcut.Arguments = Get-AgentGuiLauncherArguments $installDirectory
$shortcut.WorkingDirectory = $installDirectory
$shortcut.WindowStyle = 7
$shortcut.Description = 'Reports local Codex activity directly to the LED controller'
$shortcut.Save()

# Register before removing the legacy logon shortcut: failure retains the old autostart.
Register-AgentRecoveryTask $installDirectory $arguments
Remove-AgentStartupShortcut $installDirectory

if (-not (Test-Path -LiteralPath (Join-Path $installDirectory 'agent-config.txt'))) { @(
    "CollectorHost=$CollectorHost"
    "Port=$Port"
    "DirectHost=$DirectHost"
    "DirectPort=$DirectPort"
    "DirectHttpPort=$DirectHttpPort"
    "InstalledAt=$(Get-Date -Format o)"
) | Set-Content -LiteralPath (Join-Path $installDirectory 'agent-config.txt') -Encoding UTF8
}
$startupErrorPath = Join-Path $installDirectory 'startup-error.log'
$startupOutputPath = Join-Path $installDirectory 'startup-output.log'
$startedProcess = Start-Process -FilePath $shortcut.TargetPath -ArgumentList $shortcut.Arguments -WorkingDirectory $installDirectory -WindowStyle Hidden -RedirectStandardError $startupErrorPath -RedirectStandardOutput $startupOutputPath -PassThru
try {
    Confirm-AgentStartup -Process $startedProcess -ErrorPath $startupErrorPath -LogPath $logPath
}
finally { $startedProcess.Dispose() }

Write-Host 'LED Forge Codex LAN Agent installed and started.'
Write-Host "Agent name: $($metadata.Name)"
$displayHost = $DirectHost
try {
    $endpointPath = Join-Path $installDirectory 'controller-endpoint.json'
    if (Test-Path -LiteralPath $endpointPath) {
        $endpoint = Get-Content -LiteralPath $endpointPath -Raw | ConvertFrom-Json
        if ($endpoint.Seed -eq $DirectHost) { $displayHost = $endpoint.Host }
    }
}
catch { }
Write-Host "Set the agent color on the controller web page: http://${displayHost}:$DirectHttpPort/agents"
Write-Host "Optional LED Forge preview: ${CollectorHost}:$Port"
Write-Host "Controller search seed: ${DirectHost}:$DirectPort (DHCP recovery enabled)"
Write-Host "Direct controller HTTP: http://${displayHost}:$DirectHttpPort/agent"
Write-Host "Log: $logPath"
Write-Host 'The agent will start automatically when this user signs in.'
Write-Host 'Windows also restores a stopped supervisor within one minute while this user is signed in.'
} finally {
    if ($null -ne $recoveryTask) {
        Enable-ScheduledTask -TaskName $recoveryTask.TaskName -TaskPath '\' -ErrorAction Stop | Out-Null
    }
}
