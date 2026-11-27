[CmdletBinding(SupportsShouldProcess = $true)]
param()

$ErrorActionPreference = 'Stop'
$installed = Join-Path $env:LOCALAPPDATA 'LED Forge\Codex LAN Agent'
$metadataPath = Join-Path $installed 'agent-metadata.json'
$configPath = Join-Path $installed 'agent-config.txt'
if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    throw 'Existing agent settings not found. Run as the Windows user who installed the agent. Nothing changed.'
}
$metadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($metadata.Name)) {
    throw 'Saved agent name is missing. Restore agent-metadata.json before updating. Nothing changed.'
}
$config = @{}
foreach ($line in Get-Content -LiteralPath $configPath -Encoding UTF8) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $pair = $line.Split([char[]]'=', 2)
    if ($pair.Count -ne 2 -or $config.ContainsKey($pair[0])) {
        throw 'Invalid or duplicate saved setting. Nothing changed.'
    }
    $config[$pair[0]] = $pair[1]
}
$arguments = @{ WhatIf = [bool]$WhatIfPreference }
foreach ($key in @('CollectorHost', 'Port', 'DirectHost', 'DirectPort', 'DirectHttpPort')) {
    if (-not $config.ContainsKey($key) -or [string]::IsNullOrWhiteSpace($config[$key])) {
        throw "Missing saved setting: $key. Nothing changed."
    }
    if ($key -in @('Port', 'DirectPort', 'DirectHttpPort')) {
        $port = 0
        if (-not [int]::TryParse($config[$key], [ref]$port) -or $port -lt 1 -or $port -gt 65535) {
            throw "Invalid saved port: $key. Nothing changed."
        }
    }
    $arguments[$key] = $config[$key]
}
# Omit name/color and controller selection: retain metadata and DHCP discovery identity.
& (Join-Path $PSScriptRoot 'install_codex_lan_agent.ps1') @arguments
if (-not $?) { throw 'Agent update failed. Check the installer error above.' }
