$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'agent_update.ps1')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Assert($Value, [string]$Label) { if (-not $Value) { throw "FAIL: $Label" } }
function Reject([scriptblock]$Action, [string]$Label) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert $rejected $Label
}
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('lfau-test-' + [guid]::NewGuid().ToString('N'))
$root = Join-Path $testRoot 'agent'
$stage = Join-Path $testRoot 'stage'
$rsa = [Security.Cryptography.RSACryptoServiceProvider]::new(2048)
$rsa.PersistKeyInCsp = $false
function New-TestPackage([long]$Version = 2, [string]$BadName = '', [switch]$Missing, [switch]$Duplicate, [switch]$Symlink, [switch]$Oversized) {
    $zipPath = Join-Path $stage 'package.zip'
    [IO.File]::Delete($zipPath)
    $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in Get-AgentCodeFiles) {
            if ($Missing -and $name -eq 'weekly_tokens.ps1') { continue }
            $entry = $zip.CreateEntry($name)
            if ($Symlink -and $name -eq 'weekly_tokens.ps1') { $entry.ExternalAttributes = -1577123840 }
            $bytes = [Text.Encoding]::ASCII.GetBytes("DATA ONLY $name")
            if ($name -eq 'agent-version.txt') { $bytes = [Text.Encoding]::ASCII.GetBytes([string]$Version) }
            if ($Oversized -and $name -eq 'weekly_tokens.ps1') { $bytes = [byte[]]::new(2097153) }
            $stream = $entry.Open()
            try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
        }
        if ($BadName) { [void]$zip.CreateEntry($BadName) }
        if ($Duplicate) { [void]$zip.CreateEntry('AGENT-VERSION.TXT') }
    } finally { $zip.Dispose() }
    $size = (Get-Item -LiteralPath $zipPath).Length
    $hash = (Get-FileHash -LiteralPath $zipPath).Hash.ToLowerInvariant()
    $signature = [Convert]::ToBase64String($rsa.SignData([Text.Encoding]::UTF8.GetBytes("LFAU1`n$Version`n$size`n$hash`n"), 'SHA256'))
    $manifest = @{ schema = 1; version = $Version; size = $size; sha256 = $hash; signature = $signature } | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), $manifest)
}
try {
    [void][IO.Directory]::CreateDirectory($root)
    [void][IO.Directory]::CreateDirectory($stage)
    [IO.File]::WriteAllText((Join-Path $root 'agent-update-public.xml'), $rsa.ToXmlString($false))
    foreach ($name in Get-AgentCodeFiles) { [IO.File]::WriteAllText((Join-Path $root $name), "ORIGINAL $name") }
    [IO.File]::WriteAllText((Join-Path $root 'agent-version.txt'), '1')
    $state = @{}
    foreach ($name in @('weekly-token-state.json', 'agent-metadata.json', 'agent-config.txt', 'controller-endpoint.json', 'agent-update-identity.json', 'agent-update-public.xml')) {
        if ($name -ne 'agent-update-public.xml') { [IO.File]::WriteAllBytes((Join-Path $root $name), [byte[]]@(0, 1, 255, 13, 10)) }
        $state[$name] = (Get-FileHash -LiteralPath (Join-Path $root $name)).Hash
    }
    New-TestPackage
    Assert ((Test-AgentPackage $root $stage 1).Version -eq 2) 'Valid signature and package'
    Reject { Test-AgentPackage $root $stage 2 } 'Downgrade/replay'
    $manifest = [IO.File]::ReadAllText((Join-Path $stage 'manifest.json'))
    [IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), $manifest.Replace('"version":  2', '"version":  3').Replace('"version": 2', '"version": 3'))
    Reject { Test-AgentPackage $root $stage 1 } 'Manifest tamper'
    New-TestPackage
    [IO.File]::AppendAllText((Join-Path $stage 'package.zip'), 'tamper')
    Reject { Test-AgentPackage $root $stage 1 } 'ZIP tamper'
    foreach ($bad in @('../agent-config.txt', 'agent-version.txt:ads', 'C:/escape', 'sub/file', 'agent-update-public.xml', 'agent-update-identity.json')) {
        New-TestPackage -BadName $bad
        Reject { Test-AgentPackage $root $stage 1 } "ZIP rejected $bad"
    }
    New-TestPackage -Missing
    Reject { Test-AgentPackage $root $stage 1 } 'Missing entry'
    New-TestPackage -Duplicate
    Reject { Test-AgentPackage $root $stage 1 } 'Case-insensitive duplicate'
    New-TestPackage -Symlink
    Reject { Test-AgentPackage $root $stage 1 } 'Symlink entry'
    New-TestPackage -Oversized
    Reject { Test-AgentPackage $root $stage 1 } 'Expanded bound'
    New-TestPackage
    $failurePath = Join-Path $root '.agent-update-failed-version.txt'
    $busy = [IO.File]::Open((Join-Path $root 'weekly_tokens.ps1'),'Open','ReadWrite','None')
    try { Reject { Install-AgentStagedUpdate $root $stage } 'Backup file still busy' } finally { $busy.Dispose() }
    Assert (-not [IO.File]::Exists($failurePath)) 'Backup failure must not suppress an uninstalled release'
    Install-AgentStagedUpdate $root $stage
    Assert ((Get-AgentInstalledVersion $root) -eq 2) 'Code applied without executing it'
    Assert (Test-Path -LiteralPath (Join-Path $root '.agent-update-journal.json')) 'Pending health journal'
    Restore-AgentUpdate $root
    Assert ((Get-AgentInstalledVersion $root) -eq 1) 'Health rollback'
    Assert ([IO.File]::ReadAllText($failurePath) -ceq '2') 'Rolled-back attempted release remains suppressed'
    foreach ($name in Get-AgentCodeFiles) {
        $expected = if ($name -eq 'agent-version.txt') { '1' } else { "ORIGINAL $name" }
        Assert ([IO.File]::ReadAllText((Join-Path $root $name)) -ceq $expected) 'Original code restored'
    }
    # Inject an I/O fault after the durable journal exists, then allow recovery writes.
    $originalWriter = ${function:Write-AgentDurable}
    $script:injected = $false
    function Write-AgentDurable([string]$Path, [byte[]]$Bytes) {
        if (-not $script:injected -and $Path -eq (Join-Path $root 'weekly_tokens.ps1')) { $script:injected = $true; throw 'Injected write failure' }
        & $originalWriter $Path $Bytes
    }
    Reject { Install-AgentStagedUpdate $root $stage } 'Partial install failure'
    Set-Item Function:Write-AgentDurable $originalWriter
    Assert ((Get-AgentInstalledVersion $root) -eq 1) 'Partial failure recovered'
    foreach ($name in $state.Keys) { Assert ($state[$name] -ceq (Get-FileHash -LiteralPath (Join-Path $root $name)).Hash) "Preserved bytes $name" }
    Install-AgentStagedUpdate $root $stage
    Complete-AgentUpdate $root
    Assert (-not (Test-Path -LiteralPath (Join-Path $root '.agent-update-journal.json'))) 'Health commit'
    # Mock transport only: no sockets, payload execution, or installed-agent access.
    $script:secret = 'ab' * 32
    [IO.File]::WriteAllText((Join-Path $root 'agent-update-identity.json'), (@{ device = 'EB2198'; secret = $secret } | ConvertTo-Json))
    [IO.File]::WriteAllText((Join-Path $root 'controller-endpoint.json'), '{"Host":"10.0.0.1","HttpPort":80,"Device":"EB2198","Account":"53197668"}')
    $script:mode = 'valid'
    $script:requests = 0
    function Invoke-AgentUpdateRequest([uri]$Uri, [string]$Body, [int]$Maximum) {
        $script:requests++
        $fields = @{}
        foreach ($pair in $Body.Split('&')) { $parts = $pair.Split('='); $fields[$parts[0]] = $parts[1] }
        if ($Uri.AbsolutePath -eq '/agent-update-challenge') {
            Assert ($fields.client -cmatch '\A[0-9A-F]{8}\z' -and $fields.account -eq '53197668' -and $fields.Count -eq 2) 'Challenge contract'
            $device = if ($mode -eq 'challenge-device') { 'AABBCC' } else { 'EB2198' }
            $nonce = if ($mode -eq 'challenge-format') { 'bad' } else { '12' * 16 }
            return ,[Text.Encoding]::UTF8.GetBytes((@{ device = $device; nonce = $nonce } | ConvertTo-Json))
        }
        Assert ($Uri.AbsolutePath -eq '/agent-update-poll') 'No unsigned URL followed'
        Assert ($fields.nonce -cmatch '\A[0-9a-f]{64}\z' -and $fields.nonce.StartsWith(('12' * 16))) 'Server challenge plus fresh client nonce'
        if ($mode -eq 'old-response') { return ,$script:oldResponse }
        Assert ($fields.client -cmatch '\A[0-9A-F]{8}\z' -and $fields.version -eq '2' -and $fields.account -eq '53197668') 'Poll contract/committed version'
        $hmac = [Security.Cryptography.HMACSHA256]::new((ConvertFrom-AgentHex $secret))
        try {
            $failure = if ($fields.ContainsKey('failed')) { "FAILED`n$($fields.failed)`n" } else { '' }
            if ($mode -eq 'blocked') { Assert ($fields.failed -eq '3') 'Authenticated failed status included' }
            Assert ($fields.manage -ceq '1') 'Management capability advertised'
            $expected = -join ($hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("LFAU1-POLL`nEB2198`n$($fields.client)`n$($fields.nonce)`n2`n${failure}MANAGE`n1`n")) | ForEach-Object { $_.ToString('x2') })
            Assert ($fields.mac -ceq $expected) 'Request MAC'
            $target = if ($mode -eq 'blocked') { 3 } else { 0 }
            $nonce = $fields.nonce
            $mac = -join ($hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("LFAU1`nEB2198`n$($fields.client)`n$nonce`n$target`n")) | ForEach-Object { $_.ToString('x2') })
            if ($mode -eq 'replay') { $nonce = '00' * 16 }
            if ($mode -eq 'tamper') { $target = 3 }
            $device = if ($mode -eq 'device') { 'AABBCC' } else { 'EB2198' }
            $bytes = [Text.Encoding]::UTF8.GetBytes((@{ device = $device; version = $target; nonce = $nonce; mac = $mac } | ConvertTo-Json))
            if ($mode -eq 'valid') { $script:oldResponse = $bytes }
            return ,$bytes
        } finally { $hmac.Dispose() }
    }
    Assert (-not (Save-AgentRemoteUpdate $root $stage)) 'Authenticated no-op poll'
    $script:mode = 'old-response'
    Reject { Save-AgentRemoteUpdate $root $stage } 'Previously signed reply with replayed server challenge rejected by fresh client random'
    foreach ($badMode in @('replay', 'tamper', 'device', 'challenge-device', 'challenge-format')) { $script:mode = $badMode; Reject { Save-AgentRemoteUpdate $root $stage } "Poll $badMode" }
    $script:mode = 'blocked'
    [IO.File]::WriteAllText((Join-Path $root '.agent-update-failed-version.txt'), '3')
    Assert (-not (Save-AgentRemoteUpdate $root $stage)) 'Failed version blocked without download'
    Assert ($requests -eq 14) 'Only challenge and poll requests made'
    Write-Host 'PASS: signatures, tamper, downgrade, ZIP paths/duplicates/bounds/symlinks, partial failure, recovery, commit, state bytes, authenticated mock polling and failed-version block. No payload execution.'
} finally {
    $rsa.Dispose()
    $full = [IO.Path]::GetFullPath($testRoot)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $full.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe cleanup' }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
