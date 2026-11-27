[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$DirectHost = '10.0.80.162',
    [ValidatePattern('\A[0-9A-Fa-f]{6}\z')]
    [string]$ControllerDeviceId = 'EB2198'
)

$ErrorActionPreference = 'Stop'
$installed = Join-Path $env:LOCALAPPDATA 'LED Forge\Codex LAN Agent'
$metadataPath = Join-Path $installed 'agent-metadata.json'
$configPath = Join-Path $installed 'agent-config.txt'
if (-not (Test-Path -LiteralPath $metadataPath) -or -not (Test-Path -LiteralPath $configPath)) {
    throw 'Existing agent metadata/config not found for this Windows user. Run this under the user who installed the agent. For a new installation use install_codex_lan_agent.cmd.'
}
$metadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($metadata.Name)) {
    throw 'Saved agent name is missing. Installation unchanged; restore agent-metadata.json first.'
}
$config = @{}
foreach ($line in Get-Content -LiteralPath $configPath -Encoding UTF8) {
    $pair = $line.Split([char[]]'=', 2)
    if ($pair.Count -eq 2) {
        if ($config.ContainsKey($pair[0])) { throw "Duplicate saved setting: $($pair[0])" }
        $config[$pair[0]] = $pair[1]
    }
}
$arguments = @{
    DirectHost = $DirectHost
    ControllerDeviceId = $ControllerDeviceId
    WhatIf = [bool]$WhatIfPreference
}
foreach ($key in @('CollectorHost', 'Port', 'DirectPort', 'DirectHttpPort')) {
    if (-not $config.ContainsKey($key) -or [string]::IsNullOrWhiteSpace($config[$key])) {
        throw "Missing saved setting: $key. Installation unchanged."
    }
    $arguments[$key] = $config[$key]
}
# Do not supply metadata parameters: the installer retains the original file bytes.
& (Join-Path $PSScriptRoot 'install_codex_lan_agent.ps1') @arguments
