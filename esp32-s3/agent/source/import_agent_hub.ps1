#requires -Version 5.1
[CmdletBinding()]
param([string]$InvitationPath, [switch]$Replace)
$ErrorActionPreference = 'Stop'
if (-not $InvitationPath) { $InvitationPath = Read-Host 'Path to the trusted hub invitation JSON file' }
$root = Join-Path $env:LOCALAPPDATA 'LED Forge\Codex LAN Agent'
$destination = Join-Path $root 'hub-management.json'
$temp = Join-Path $root ('.hub-import-' + [guid]::NewGuid().ToString('N'))
$lockPath = Join-Path $root '.management-binding.lock'
$requiredPath = Join-Path $root '.hub-management-required'
function Assert-PlainImportPath([string]$path) {
    for ($p = [IO.Path]::GetFullPath($path); $p; $p = [IO.Path]::GetDirectoryName($p)) {
        if ((Test-Path -LiteralPath $p) -and ((Get-Item -Force -LiteralPath $p).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Linked import path rejected.'
        }
    }
}
foreach ($path in $destination,$lockPath,$requiredPath,$InvitationPath) { Assert-PlainImportPath $path }
if (-not [IO.File]::Exists((Join-Path $root 'codex_lan_agent.ps1'))) { throw 'Current-user agent installation not found.' }
try {
    if ((Get-Item -LiteralPath $InvitationPath).Length -gt 4096) { throw 'Size' }
    $b = [IO.File]::ReadAllText([IO.Path]::GetFullPath($InvitationPath)) | ConvertFrom-Json -ErrorAction Stop
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $client = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes("$env:COMPUTERNAME-$env:USERNAME")),0,4).Replace('-','') }
    finally { $sha.Dispose() }
    $ip = $null
    if (($b.PSObject.Properties.Name | Sort-Object) -join ',' -cne 'Account,client,Device,Host,HttpPort,secret' -or
        $b.Host -isnot [string] -or -not [Net.IPAddress]::TryParse($b.Host,[ref]$ip) -or $ip.AddressFamily -ne 'InterNetwork' -or
        $ip.ToString() -cne $b.Host -or $b.Host -notmatch '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.|127\.)' -or
        ($b.HttpPort -isnot [int] -and $b.HttpPort -isnot [long]) -or $b.HttpPort -lt 1 -or $b.HttpPort -gt 65535 -or
        $b.Device -isnot [string] -or $b.Device -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or $b.Account -isnot [string] -or $b.Account -cne '53197668' -or
        $b.secret -isnot [string] -or $b.secret -cnotmatch '\A[0-9a-f]{64}\z' -or $b.client -isnot [string] -or $b.client -cne $client) { throw 'Fields' }
} catch { throw 'Invalid hub invitation for this PC and Windows user.' }
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
$acl = [Security.AccessControl.FileSecurity]::new()
$acl.SetOwner($sid)
$acl.SetAccessRuleProtection($true,$false)
$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','Allow'))
$b.Device = $b.Device.ToUpperInvariant()
$bytes = [Text.Encoding]::UTF8.GetBytes(($b | ConvertTo-Json -Compress))
# Existing installations can provision this file without loading or replacing their installed code.
try { $lock = [IO.File]::Open($lockPath,'OpenOrCreate','ReadWrite','None') }
catch { throw 'Management operation in progress or installation inaccessible. Retry the local import.' }
try {
    if ([IO.File]::Exists($destination)) {
        if (-not $Replace) { throw 'A hub is already pinned. Use -Replace to authorize a new binding.' }
        $old = [IO.File]::GetAccessControl($destination)
        if (-not $old.GetOwner([Security.Principal.SecurityIdentifier]).Equals($sid)) { throw 'Foreign binding owner.' }
        [IO.File]::SetAccessControl($destination,$acl)
    }
    $stream = [IO.FileStream]::new($temp,[IO.FileMode]::CreateNew,[Security.AccessControl.FileSystemRights]::FullControl,
        [IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough,$acl)
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    # Losing the pinned file after provisioning must never switch the agent back to controller enrollment.
    if (-not [IO.File]::Exists($requiredPath)) {
        $marker = [IO.File]::Open($requiredPath,'CreateNew','Write','None')
        try { $marker.WriteByte(1); $marker.Flush($true) } finally { $marker.Dispose() }
    }
    if ([IO.File]::Exists($destination)) { [IO.File]::Replace($temp,$destination,[Management.Automation.Language.NullString]::Value) }
    else { [IO.File]::Move($temp,$destination) }
} finally {
    $lock.Dispose()
    [IO.File]::Delete($temp)
}
Write-Host "Hub management invitation stored for client $client at $($b.Host):$($b.HttpPort)."
