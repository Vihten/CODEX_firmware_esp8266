[CmdletBinding(SupportsShouldProcess)]
param([Parameter(Mandatory)][ValidateSet('Enable','Remove')][string]$Action)

$ErrorActionPreference = 'Stop'
$group = 'LED Forge Direct Peer Chat'
$program = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$ports = [ordered]@{ UDP = 4214; TCP = 4215 }

foreach ($protocol in $ports.Keys) {
    $port = $ports[$protocol]
    $name = "LEDForge.DirectPeerChat.$protocol.$port"
    if (-not $PSCmdlet.ShouldProcess($name, $Action)) { continue }
    $existing = Get-NetFirewallRule -Name $name -PolicyStore PersistentStore -ErrorAction SilentlyContinue
    if ($existing -and $existing.Group -cne $group) { throw "Rule name belongs to another group: $name" }
    if ($Action -eq 'Remove') {
        if ($existing) { Remove-NetFirewallRule -Name $name -PolicyStore PersistentStore }
        continue
    }
    $settings = @{
        Name = $name; PolicyStore = 'PersistentStore'; Direction = 'Inbound'; Action = 'Allow'
        Profile = @('Domain','Private'); RemoteAddress = 'LocalSubnet'; Protocol = $protocol
        LocalPort = $port; Program = $program; EdgeTraversalPolicy = 'Block'; Enabled = 'True'
    }
    if ($existing) { Set-NetFirewallRule @settings }
    else { New-NetFirewallRule @settings -DisplayName "$group ($protocol $port)" -Group $group | Out-Null }
}
