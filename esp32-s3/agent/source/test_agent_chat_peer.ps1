#requires -Version 5.1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'agent_chat_peer.ps1')
function Check($Condition,$Label) { if (-not $Condition) { throw $Label } }
function Reject([scriptblock]$Action,$Label) {
    try { $null = & $Action } catch { return }
    throw "Expected rejection: $Label"
}
function FixtureId {
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
    do {
        $candidate = [guid]::NewGuid().ToString('N').Substring(0,8).ToUpperInvariant()
    } while (Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -ceq "CN=Codex-LAN-Peer-v1-$candidate")
    $candidate
}
# Receives at most one loopback connection on a withdrawn fixture peer's port.
function Test-ReceiptBoundary($Address,$Port,$Certificate,$ExpectedClientPin,[bool]$ExpectFrame) {
    $listener = [Net.Sockets.TcpListener]::new([ipaddress]$Address,$Port)
    $listener.Start(1)
    $worker = [powershell]::Create()
    try {
        [void]$worker.AddScript({
            param($listener,$cert,$pin)
            $ErrorActionPreference = 'Stop'
            $tcp = $listener.AcceptTcpClient()
            try {
                $tcp.ReceiveTimeout = 2500
                $tcp.SendTimeout = 2500
                $tls = [Net.Security.SslStream]::new($tcp.GetStream(),$false,{
                    param($sender,$remote,$chain,$errors)
                    if (-not $remote) { return $false }
                    $hash = [Security.Cryptography.SHA256]::Create()
                    try { [BitConverter]::ToString($hash.ComputeHash($remote.GetRawCertData())).Replace('-','') -ceq $pin }
                    finally { $hash.Dispose() }
                })
                try {
                    $tls.AuthenticateAsServer($cert,$true,[Security.Authentication.SslProtocols]::Tls12,$false)
                    $hi = $tls.ReadByte()
                    $lo = $tls.ReadByte()
                    if ($hi -lt 0 -or $lo -lt 0) { return 'no frame' }
                    $len = 256*$hi + $lo
                    if ($len -lt 1 -or $len -gt 2048) { throw 'Invalid fixture input' }
                    $r = [IO.BinaryReader]::new($tls)
                    $data = $r.ReadBytes($len)
                    if ($data.Length -ne $len) { throw 'Truncated fixture input' }
                    $request = [Text.Encoding]::UTF8.GetString($data).Split('|')
                    $ack = 'LFPEER1|ACK|{0}|{1}|{2}' -f $request[3],$request[2],[guid]::NewGuid()
                    $bytes = [Text.Encoding]::UTF8.GetBytes($ack)
                    $tls.WriteByte(0)
                    $tls.WriteByte($bytes.Length)
                    $tls.Write($bytes,0,$bytes.Length)
                    $tls.Flush()
                    'wrong receipt sent'
                } catch [Security.Authentication.AuthenticationException] { 'no frame' }
                  catch [IO.IOException] { 'no frame' }
                finally { $tls.Dispose() }
            } finally { $tcp.Close() }
        }).AddArgument($listener).AddArgument($Certificate).AddArgument($ExpectedClientPin)
        $job = $worker.BeginInvoke()
        Reject { Send-Fixture } 'invalid remote certificate or receipt'
        Check ($job.AsyncWaitHandle.WaitOne(6000)) 'Probe timed out'
        $result = $worker.EndInvoke($job)
        $expected = if ($ExpectFrame) { 'wrong receipt sent' } else { 'no frame' }
        Check ($result.Count -eq 1 -and $result[0] -ceq $expected) "Unexpected probe result ($result)"
    } finally {
        $listener.Stop()
        $worker.Dispose()
    }
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('lf-peer-' + [guid]::NewGuid().ToString('N'))
$aId = FixtureId
$bId = FixtureId
$addresses = '127.0.0.2','127.0.0.3'
$reservation = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([ipaddress]$addresses[0],0))
$port = $reservation.Client.LocalEndPoint.Port
$reservation.Close()
function Start-Fixture($Index,$Id,$Name) {
    $dir = Join-Path $root $Id
    foreach ($sub in 'inbox','seen') { [void][IO.Directory]::CreateDirectory((Join-Path $dir $sub)) }
    Start-ChatPeerRuntime $dir $Id $Name -BindAddress $addresses[$Index] -DiscoveryAddress $addresses[1-$Index] -DiscoveryPort $port -TcpPort 0
}
try {
    $a = Start-Fixture 0 $aId 'Fixture A'
    $b = Start-Fixture 1 $bId 'Fixture B'
    $limit = [datetime]::UtcNow.AddSeconds(14)
    while (-not ((Get-ChatPeerCandidates $a) -and (Get-ChatPeerCandidates $b))) {
        Check ([datetime]::UtcNow -lt $limit) 'Signed loopback discovery timed out'
        Start-Sleep -Milliseconds 80
    }
    $body = "fixture $([char]0x2603)"
    $uuid = [guid]::NewGuid().ToString()
    function Send-Fixture { Send-ChatPeerMessage $a $bId $uuid $body }
    Reject { Send-Fixture } 'unenrolled target'
    Reject { Set-ChatPeerTrust $a $aId 'Fixture A' $a.Pin } 'self trust'
    Set-ChatPeerTrust $a $bId 'Fixture B' $b.Pin
    Reject { Send-Fixture } 'unenrolled sender'
    Set-ChatPeerTrust $b $aId 'Forged Name' $a.Pin
    Reject { Send-Fixture } 'name mismatch'
    Remove-ChatPeerTrust $b $aId
    Set-ChatPeerTrust $b '00000000' 'Fixture A' $a.Pin
    Reject { Send-Fixture } 'sender ID mismatch'
    Reject { Set-ChatPeerTrust $b $aId 'Fixture A' $a.Pin } 'duplicate pin'
    Remove-ChatPeerTrust $b '00000000'
    Set-ChatPeerTrust $b $aId 'Fixture A' $a.Pin
    Check ((Get-ChatPeerRoster $a).online) 'Trusted peer offline'
    # A paused TLS handshake must not occupy every receiver slot.
    $idle = [Net.Sockets.TcpClient]::new()
    try {
        $idle.Connect($addresses[1],$b.TcpPort)
        Check ((Send-Fixture) -ceq $uuid) 'Receipt mismatch'
        $idle.ReceiveTimeout = 6500
        Check ($idle.GetStream().ReadByte() -eq -1) 'Handshake deadline not enforced'
    } finally { $idle.Close() }
    $file = Join-Path $root "$bId/inbox/$uuid.json"
    $record = [IO.File]::ReadAllText($file) | ConvertFrom-Json
    Check ($record.sender_id -ceq $aId -and $record.sender_name -ceq 'Fixture A' -and $record.text -ceq $body) 'Mailbox identity or content mismatch'
    Check ((Send-Fixture) -ceq $uuid) 'Inbox retry'
    [IO.File]::Move($file,(Join-Path $root "$bId/seen/$uuid.json"))
    Check ((Send-Fixture) -ceq $uuid -and -not [IO.File]::Exists($file)) 'Seen retry'
    Reject { Send-ChatPeerMessage $a $bId $uuid 'different' } 'conflicting UUID'
    Reject { Send-ChatPeerMessage $a $aId $uuid $body } 'self delivery'
    Reject { Send-ChatPeerMessage $a $bId $uuid ('x'*513) } 'oversize text'
    $reverse = [guid]::NewGuid().ToString()
    Check ((Send-ChatPeerMessage $b $aId $reverse 'reverse fixture') -ceq $reverse) 'Reverse delivery'
    $pin = $b.Pin
    $oldPort = $b.TcpPort
    Stop-ChatPeerRuntime $b
    Test-ReceiptBoundary $addresses[1] $oldPort (Get-ChatPeerCertificate $aId) $a.Pin $false
    Test-ReceiptBoundary $addresses[1] $oldPort (Get-ChatPeerCertificate $bId) $a.Pin $true
    $b = Start-Fixture 1 $bId 'Fixture B'
    Check ($b.Pin -ceq $pin) 'Certificate changed across restart'
    'PASS: loopback UDP discovery, bilateral TLS pins, sender ID/name checks, validated receipt, durable deduplication, bounded text, reverse delivery and restart.'
} finally {
    Stop-ChatPeerRuntime $a
    Stop-ChatPeerRuntime $b
    foreach ($id in $aId,$bId) {
        Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -ceq "CN=Codex-LAN-Peer-v1-$id" | ForEach-Object { Remove-Item -LiteralPath ("Cert:\CurrentUser\My\" + $_.Thumbprint) -DeleteKey -Force }
    }
    $full = [IO.Path]::GetFullPath($root)
    if ([IO.Path]::GetDirectoryName($full) -eq [IO.Path]::GetTempPath().TrimEnd('\') -and
        [IO.Path]::GetFileName($full) -match '^lf-peer-[a-f0-9]{32}$' -and [IO.Directory]::Exists($full)) {
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
