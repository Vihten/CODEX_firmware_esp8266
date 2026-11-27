$ErrorActionPreference = 'Stop'
$rules = @{}
function Get-NetFirewallRule($Name,$PolicyStore,$ErrorAction) {
    if ($PolicyStore -ne 'PersistentStore') { throw 'Wrong store' }
    $rules[$Name]
}
function New-NetFirewallRule($Name,$PolicyStore,$Direction,$Action,$Profile,$RemoteAddress,$Protocol,
    $LocalPort,$Program,$EdgeTraversalPolicy,$Enabled,$DisplayName,$Group) {
    if ($rules.ContainsKey($Name)) { throw 'Duplicate rule' }
    $rules[$Name] = @{} + $PSBoundParameters
}
function Set-NetFirewallRule($Name,$PolicyStore,$Direction,$Action,$Profile,$RemoteAddress,$Protocol,
    $LocalPort,$Program,$EdgeTraversalPolicy,$Enabled) {
    foreach ($key in $PSBoundParameters.Keys) { $rules[$Name][$key] = $PSBoundParameters[$key] }
}
function Remove-NetFirewallRule($Name,$PolicyStore) { $rules.Remove($Name) }
$scriptPath = Join-Path $PSScriptRoot 'configure_peer_firewall.ps1'
& $scriptPath -Action Enable -WhatIf
if ($rules.Count) { throw 'WhatIf created rules' }
& $scriptPath -Action Enable
& $scriptPath -Action Enable
if ($rules.Count -ne 2) { throw 'Expected two rules' }
foreach ($protocol in @('TCP','UDP')) {
    $port = if ($protocol -eq 'TCP') { 4215 } else { 4214 }
    $rule = $rules["LEDForge.DirectPeerChat.$protocol.$port"]
    if ($rule.LocalPort -ne $port -or $rule.Protocol -ne $protocol -or $rule.Enabled -ne 'True' -or
        $rule.Program -ne (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe') -or
        ($rule.Profile -join ',') -ne 'Domain,Private' -or $rule.RemoteAddress -ne 'LocalSubnet' -or
        $rule.EdgeTraversalPolicy -ne 'Block' -or $rule.Direction -ne 'Inbound' -or $rule.Action -ne 'Allow') {
        throw "Unexpected $protocol firewall policy"
    }
}
& $scriptPath -Action Remove
& $scriptPath -Action Remove
if ($rules.Count) { throw 'Removal failed' }
Write-Host 'PASS peer firewall creation, repeat enable, scope, preview and removal (mocked NetSecurity).'
