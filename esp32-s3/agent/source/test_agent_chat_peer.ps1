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
function Start-Fixture($Index,$Id,$Name,$Target=$addresses[1-$Index]) {
    $dir = Join-Path $root $Id
    foreach ($sub in 'inbox','seen') { [void][IO.Directory]::CreateDirectory((Join-Path $dir $sub)) }
    Start-ChatPeerRuntime $dir $Id $Name -BindAddress $addresses[$Index] -DiscoveryAddress $Target -DiscoveryPort $port -TcpPort 0
}
function Test-BeaconReplies($ReceiverPin,$SenderPort) {
    # The legacy announcer transmits from an ephemeral port and listens on the discovery port.
    $receive = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([ipaddress]$addresses[1],$port))
    $send = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([ipaddress]$addresses[1],0))
    $cert = Get-ChatPeerCertificate $bId
    $key = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($cert)
    try {
        function Send-Beacon($Marker, [int]$AgeSeconds=0, [switch]$InvalidSignature) {
            $stamp = $Marker + [DateTimeOffset]::UtcNow.AddSeconds(-$AgeSeconds).ToUnixTimeMilliseconds().ToString()
            $fields = 'LFPEER1',$bId,[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('Fixture B')),$SenderPort,$stamp,[Convert]::ToBase64String($cert.RawData)
            $data = [Text.Encoding]::UTF8.GetBytes(($fields -join '|'))
            $sig = $key.SignData($data,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
            if ($InvalidSignature) { $sig[0] = $sig[0] -bxor 1 }
            $bytes = [Text.Encoding]::UTF8.GetBytes((($fields + [Convert]::ToBase64String($sig)) -join '|'))
            [void]$send.Send($bytes,$bytes.Length,[Net.IPEndPoint]::new([ipaddress]$addresses[0],$port))
        }
        function Read-Reply {
            $wait = [Diagnostics.Stopwatch]::StartNew()
            while ($wait.ElapsedMilliseconds -lt 1100) {
                if (-not $receive.Client.Poll(50000,[Net.Sockets.SelectMode]::SelectRead)) { continue }
                $from = [Net.IPEndPoint]::new([ipaddress]::Any,0)
                $p = [Text.Encoding]::UTF8.GetString($receive.Receive([ref]$from)).Split('|')
                if ($p[4][0] -ne '+') { continue } # A's regular LFPEER1 advertisements.
                $t = 0L
                Check ($p.Count -eq 7 -and $p[0] -ceq 'LFPEER1' -and $p[1] -ceq $aId -and [long]::TryParse($p[4],[ref]$t) -and $t -gt 0) 'Legacy beacon shape'
                $sha = [Security.Cryptography.SHA256]::Create()
                try { Check ([BitConverter]::ToString($sha.ComputeHash([Convert]::FromBase64String($p[5]))).Replace('-','') -ceq $ReceiverPin) 'Reply certificate mismatch' }
                finally { $sha.Dispose() }
                return $true
            }
            return $false
        }
        Set-ChatPeerTrust $a $bId 'Fixture B' ('0'*64)
        Send-Beacon ''
        Check (-not (Read-Reply)) 'Conflicting certificate received a discovery reply'
        Check ((Get-ChatPeerRoster $a).pin -ceq ('0'*64) -and -not (Get-ChatPeerRoster $a).online) 'Discovery replaced a pin'
        Remove-ChatPeerTrust $a $bId
        Send-Beacon '' -AgeSeconds 60
        Check (-not (Read-Reply)) 'Expired candidate beacon received a reply'
        Send-Beacon '' -InvalidSignature
        Check (-not (Read-Reply)) 'Invalid candidate signature received a reply'
        Send-Beacon '+'
        Check (-not (Read-Reply)) 'Terminal reply triggered another reply'
        Send-Beacon ''
        Check (Read-Reply) 'Legacy announcement received no compatible reply on the discovery port'
        Send-Beacon ''
        Check (-not (Read-Reply)) 'Reply cooldown bypassed'
    } finally { $key.Dispose(); $send.Close(); $receive.Close() }
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
    # Fresh caches. A's periodic announcements reach B; B's go only to a loopback sink.
    Stop-ChatPeerRuntime $a
    $sink = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([ipaddress]'127.0.0.4',$port))
    $b = Start-Fixture 1 $bId 'Fixture B' '127.0.0.4'
    Check ($b.Pin -ceq $pin) 'Certificate changed across restart'
    $a = Start-Fixture 0 $aId 'Fixture A'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while (-not ((Get-ChatPeerCandidates $a) -and (Get-ChatPeerCandidates $b))) {
        Check ($clock.Elapsed.TotalSeconds -lt 10) 'Unpaired asymmetric discovery timed out'
        Start-Sleep -Milliseconds 100
    }
    $clock.Restart()
    while ($clock.Elapsed.TotalSeconds -lt 21) {
        $atA = Get-ChatPeerCandidates $a
        $atB = Get-ChatPeerCandidates $b
        Check ($atA.id -ceq $bId -and $atA.pin -ceq $b.Pin -and $atA.address -eq $addresses[1]) 'Unpaired B candidate missing at A'
        Check ($atB.id -ceq $aId -and $atB.pin -ceq $a.Pin -and $atB.address -eq $addresses[0]) 'Unpaired A candidate missing at B'
        Start-Sleep -Milliseconds 200
    }
    Check (-not ((Get-ChatPeerRoster $a) -or (Get-ChatPeerRoster $b))) 'Discovery granted trust'
    $probeId = [guid]::NewGuid().ToString()
    Reject { Send-ChatPeerMessage $a $bId $probeId 'before approval' } 'unpaired A to B'
    Reject { Send-ChatPeerMessage $b $aId $probeId 'before approval' } 'unpaired B to A'
    Set-ChatPeerTrust $a $bId 'Fixture B' $b.Pin
    Reject { Send-ChatPeerMessage $a $bId $probeId 'before approval' } 'missing receiver approval'
    Reject { Send-ChatPeerMessage $b $aId $probeId 'before approval' } 'missing sender approval'
    foreach ($id in $aId,$bId) {
        Check (-not [IO.File]::Exists("$root/$id/inbox/$probeId.json")) 'Unapproved message reached the inbox'
    }
    Set-ChatPeerTrust $b $aId 'Fixture A' $a.Pin
    $clock.Restart()
    while ($clock.Elapsed.TotalSeconds -lt 21) {
        Check ((Get-ChatPeerRoster $a).online -and (Get-ChatPeerRoster $b).online) 'Asymmetric presence expired'
        Start-Sleep -Milliseconds 200
    }
    Check ($sink.Available -gt 0) 'Missing periodic announcements at the loopback sink'
    $fresh = [guid]::NewGuid().ToString()
    Check ((Send-ChatPeerMessage $a $bId $fresh 'one-way discovery') -ceq $fresh) 'Delivery after asymmetric discovery TTL'
    $senderPort = $b.TcpPort
    Stop-ChatPeerRuntime $b
    Test-BeaconReplies $a.Pin $senderPort
    'PASS: one-way discovery kept unpaired candidates and approved peers visible past TTL; delivery required bilateral approval.'
    'PASS: loopback UDP discovery, bilateral TLS pins, sender ID/name checks, validated receipt, durable deduplication, bounded text, reverse delivery and restart.'
} finally {
    if ($sink) { $sink.Close() }
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
