[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('Enable', 'Remove')][string]$Action = 'Enable',
    [ValidateRange(1,65535)][int]$Port = 4220,
    [string]$PythonWindowed
)

$ErrorActionPreference = 'Stop'
$name = "LEDForge.AgentHub.TCP.$Port"
$group = 'LED Forge Agent Hub'
if (-not $PSCmdlet.ShouldProcess("$name (private/domain LAN)", $Action)) { return }
$existing = Get-NetFirewallRule -Name $name -PolicyStore PersistentStore -ErrorAction SilentlyContinue
if ($existing -and $existing.Group -cne $group) { throw "Rule name belongs to another group: $name" }
if ($Action -eq 'Remove') {
    if ($existing) { Remove-NetFirewallRule -Name $name -PolicyStore PersistentStore }
    return
}
if (-not $PythonWindowed) {
    $python = Get-Command python.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $runtime = & $python.Source -c 'import sys;print(sys.executable)'
    if ($LASTEXITCODE -ne 0 -or -not $runtime) { throw 'Select the Python runtime used by the hub.' }
    $PythonWindowed = Join-Path (Split-Path $runtime -Parent) 'pythonw.exe'
}
$pythonw = Get-Item -LiteralPath $PythonWindowed
if ($pythonw.Name -ine 'pythonw.exe' -or $pythonw.PSIsContainer) { throw 'Expected pythonw.exe' }
$settings = @{
    Name = $name; PolicyStore = 'PersistentStore'; Direction = 'Inbound'; Action = 'Allow'
    Profile = @('Domain','Private'); RemoteAddress = 'LocalSubnet'; Protocol = 'TCP'
    LocalPort = $Port; Program = $pythonw.FullName; EdgeTraversalPolicy = 'Block'; Enabled = 'True'
}
if ($existing) { Set-NetFirewallRule @settings }
else { New-NetFirewallRule @settings -DisplayName "LED Forge Agent Hub (TCP $Port)" -Group $group | Out-Null }
