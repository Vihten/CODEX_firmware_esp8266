$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script as Administrator on the LED Forge collector PC.'
}

$displayName = 'LED Forge Codex LAN Activity'
Get-NetFirewallRule -DisplayName $displayName -ErrorAction SilentlyContinue |
    Remove-NetFirewallRule
New-NetFirewallRule `
    -DisplayName $displayName `
    -Direction Inbound `
    -Action Allow `
    -Protocol UDP `
    -LocalPort 4212 `
    -Profile Private `
    -RemoteAddress LocalSubnet | Out-Null

Write-Host 'Inbound UDP 4212 is allowed from the private local subnet.'
