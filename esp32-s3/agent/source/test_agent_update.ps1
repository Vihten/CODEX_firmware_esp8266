$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('ledforge-update-' + [guid]::NewGuid().ToString('N'))
$oldLocalAppData = $env:LOCALAPPDATA
function Assert-True($value, [string]$label) { if (-not $value) { throw $label } }
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $env:LOCALAPPDATA = $root
    $installed = Join-Path $root 'LED Forge\Codex LAN Agent'
    New-Item -ItemType Directory -Path $installed -Force | Out-Null
    $metadata = Join-Path $installed 'agent-metadata.json'
    $config = Join-Path $installed 'agent-config.txt'
    '{"Name":"Existing name","Extra":{"Keep":42}}' | Set-Content -LiteralPath $metadata -Encoding UTF8
    $settings = @('CollectorHost=255.255.255.255', 'Port=4512', 'DirectHost=10.0.80.163',
        'DirectPort=4510', 'DirectHttpPort=8080', 'InstalledAt=2026-01-01') -join "`n"
    $settings | Set-Content -LiteralPath $config -Encoding UTF8
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'update_codex_lan_agent.ps1') -Destination $root
    # Capture installer arguments; never install or terminate a process in this test.
    @'
[CmdletBinding(SupportsShouldProcess = $true)]
param($CollectorHost, $Port, $DirectHost, $DirectPort, $DirectHttpPort)
$PSBoundParameters | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'captured.json') -WhatIf:$false
'@ | Set-Content -LiteralPath (Join-Path $root 'install_codex_lan_agent.ps1') -Encoding UTF8
    $before = (Get-FileHash -LiteralPath $metadata).Hash
    $wrapper = Join-Path $root 'update_codex_lan_agent.ps1'
    & $wrapper -WhatIf
    $captured = Get-Content -LiteralPath (Join-Path $root 'captured.json') -Raw | ConvertFrom-Json
    Assert-True ($captured.WhatIf.IsPresent) 'WhatIf forwarded'
    Assert-True ($captured.CollectorHost -eq '255.255.255.255' -and $captured.DirectHost -eq '10.0.80.163') 'Hosts retained'
    Assert-True ($captured.Port -eq 4512 -and $captured.DirectPort -eq 4510 -and $captured.DirectHttpPort -eq 8080) 'Ports retained'
    Assert-True ($before -eq (Get-FileHash -LiteralPath $metadata).Hash) 'Metadata bytes unchanged'
    & $wrapper
    $captured = Get-Content -LiteralPath (Join-Path $root 'captured.json') -Raw | ConvertFrom-Json
    Assert-True (-not $captured.WhatIf.IsPresent) 'Normal update forwarded'
    foreach ($invalid in @($settings.Replace('Port=4512', 'MissingPort=4512'), ($settings + "`nPort=4512"), $settings.Replace('Port=4512', 'Port=0'))) {
        $invalid | Set-Content -LiteralPath $config -Encoding UTF8
        $rejected = $false
        try { & $wrapper } catch { $rejected = $true }
        Assert-True $rejected 'Invalid settings rejected without mutation'
    }
    $settings | Set-Content -LiteralPath $config -Encoding UTF8
    '{}' | Set-Content -LiteralPath $metadata -Encoding UTF8
    $rejected = $false
    try { & $wrapper } catch { $rejected = $true }
    Assert-True $rejected 'Missing name rejected'
    Write-Host 'PASS: update retains hosts/ports and metadata; supports WhatIf; rejects missing name and invalid/duplicate config. No installation.'
}
finally {
    $env:LOCALAPPDATA = $oldLocalAppData
    $resolved = [IO.Path]::GetFullPath($root)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
