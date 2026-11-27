#requires -Version 5.1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'agent_update.ps1')
. (Join-Path $PSScriptRoot 'agent_chat_ipc.ps1')
function Check($condition,$label) { if (-not $condition) { throw $label } }
function Reject([scriptblock]$action) { try { & $action } catch { return }; throw 'Expected failure' }
$root = Join-Path ([IO.Path]::GetTempPath()) ('chat-fixture-' + [guid]::NewGuid())
function Mode($value) { [IO.File]::WriteAllText("$root/mode",$value) }
function Queue($id,$targets) {
    Write-ChatObject "$root/outbox/$id.json" @{id=$id;recipient_ids=$targets;text=("test" + [char]10 + [char]0x2603)}
}
try {
    Initialize-ChatMailbox $root
    $ctx = @{Client='01020304';Seq=0L}
    $key = Get-ChatSecret $root '112233' $ctx.Client -Create
    $hex = [BitConverter]::ToString($key).Replace('-','')
    [IO.File]::WriteAllText("$root/fixture.key",$hex)
    Mode idle
    $fixture = Join-Path $PSScriptRoot 'test_chat_relay_fixture.py'
    $proc = Start-Process python -ArgumentList ('"' + $fixture + '" "' + $root + '"') -WindowStyle Hidden -PassThru
    $deadline = [datetime]::UtcNow.AddSeconds(8)
    while (-not [IO.File]::Exists("$root/port")) {
        if ($proc.HasExited -or [datetime]::UtcNow -gt $deadline) { throw 'Fixture startup failed' }
        Start-Sleep -Milliseconds 30
    }
    $ep = "$root/endpoint.json"
    @{Host='127.0.0.1';HttpPort=[int][IO.File]::ReadAllText("$root/port");Account='53197668';Device='112233'} | ConvertTo-Json | Set-Content -Encoding UTF8 $ep
    # Fault injection at the pinned-peer API boundary. HTTP still uses the real signed fixture.
    $peerId = 'AABBCCDD'
    $pin = '1' * 64
    $ctx.Peer = @{Pin=('0' * 64)}
    $ctx.Peer | Add-Member ScriptMethod RestoreRoute { param($id,$pin,$address)
        $script:restoredRoute = "$id/$pin/$address"
    }
    $ctx.Name = 'Sender'
    $sent = @{}
    $script:loseReceipt = $false
    function Get-ChatPeerCandidates($runtime) { [pscustomobject]@{id=$peerId;name='Desk';pin=$pin;address='127.0.0.2'} }
    $script:peerOnline = $true
    function Get-ChatPeerRoster($runtime) { [pscustomobject]@{id=$peerId;name='Desk';online=$script:peerOnline;address='127.0.0.2'} }
    function Set-ChatPeerTrust($runtime,$id,$name,$fingerprint) { Check ($fingerprint -ceq $pin) 'Wrong applied pin' }
    function Remove-ChatPeerTrust($runtime,$id) {}
    function Send-ChatPeerMessage($runtime,$to,$uuid,$text) {
        Check ($to -ceq $peerId -and $text -ceq ("test" + [char]10 + [char]0x2603)) 'Wrong peer or body'
        $sent[$uuid] = 1 + [int]$sent[$uuid]
        if ($script:loseReceipt) { throw 'Lost receipt' }
        $uuid
    }
    function Cycle { Invoke-ChatCycle $root $ep $ctx }
    function Journal($id) { Read-ChatObject "$root/deliveries/$id.json" }
    function RetryNow($id) {
        foreach ($folder in 'deliveries','board-attempts') {
            $path = "$root/$folder/$id.json"
            if ([IO.File]::Exists($path)) {
                $rec = Read-ChatObject $path
                foreach ($target in $rec.targets) { $target.retry = 0L }
                Write-ChatObject $path $rec
            }
        }
    }
    function Calls { if ([IO.File]::Exists("$root/calls")) { (Get-Content "$root/calls") -join ',' } else { '' } }
    Cycle
    $snapshot = Read-ChatObject "$root/roster.json"
    Check ($snapshot.self_fingerprint -ceq $ctx.Peer.Pin -and $snapshot.candidates[0].fingerprint -ceq $pin) 'Missing verification metadata'
    Check ($snapshot.agents.id -cnotcontains $peerId) 'Unverified advertisement enabled'
    Write-ChatObject "$root/trusted/$peerId.json" @{id=$peerId;name='Desk';fingerprint=$pin}
    Cycle
    $stamp = [IO.File]::GetLastWriteTimeUtc("$root/roster.json")
    Cycle
    Check ((Calls) -ceq '' -and [IO.File]::GetLastWriteTimeUtc("$root/roster.json") -eq $stamp) 'Idle HTTP or roster churn'

    # No endpoint or board key is required to send directly.
    [IO.File]::Move($ep,"$ep.off")
    $id = [guid]::NewGuid().ToString()
    Queue $id @($peerId)
    Cycle
    Check ($sent[$id] -eq 1 -and -not [IO.File]::Exists("$root/outbox/$id.json") -and (Calls) -ceq '') 'Controller-free delivery failed'
    $mixed = [guid]::NewGuid().ToString()
    Queue $mixed @('board',$peerId)
    Cycle
    $rec = Journal $mixed
    Check ($sent[$mixed] -eq 1 -and $rec.targets[0].state -ceq 'pending' -and $rec.targets[1].state -ceq 'delivered') 'Mixed selection lost'
    Check ((Read-ChatObject "$root/outbox/$mixed.json").recipient_ids.Count -eq 2) 'Original selections changed'
    [IO.File]::Move("$ep.off",$ep)
    RetryNow $mixed
    Cycle
    Check ($sent[$mixed] -eq 1 -and (Calls) -ceq 'state,send' -and -not [IO.File]::Exists("$root/outbox/$mixed.json")) 'Board recovery duplicated peer or ignored receipt'

    # Retry after a lost peer receipt keeps the UUID, across a fresh worker context.
    $id = [guid]::NewGuid().ToString()
    Queue $id @($peerId)
    $script:loseReceipt = $true
    Cycle
    Check ((Journal $id).targets[0].state -ceq 'pending') 'Uncertain peer send lost'
    $ctx = @{Client=$ctx.Client;Name=$ctx.Name;Peer=$ctx.Peer;Seq=0L}
    $script:loseReceipt = $false
    RetryNow $id
    Cycle
    Check ($script:restoredRoute -ceq "$peerId/$pin/127.0.0.2") 'Pinned address not restored after transport restart'
    Check ($sent[$id] -eq 2 -and (Journal $id).targets[0].state -ceq 'delivered') 'Restart retry failed'

    $legacy = [guid]::NewGuid().ToString()
    Queue $legacy @($peerId,'board')
    Write-ChatObject "$root/attempts/$legacy.json" @{boot='0123456789abcdef';digest='old';started=1}
    Cycle
    Check ([IO.File]::Exists("$root/failed/$legacy.json") -and [IO.File]::Exists("$root/attempts/$legacy.json") -and
        -not $sent.ContainsKey($legacy) -and (Calls) -ceq 'state,send' -and (Journal $legacy).targets.state -notcontains 'pending') 'Replayed old relay attempt'

    $id = [guid]::NewGuid().ToString()
    Queue $id @('board')
    Mode unsigned
    Cycle
    Check ((Journal $id).targets[0].state -ceq 'pending') 'Unsigned receipt accepted'
    Mode reboot
    RetryNow $id
    Cycle
    Check ((Journal $id).targets[0].state -ceq 'uncertain' -and (Calls) -ceq 'state,send,state,send,state') 'Replayed uncertain board send after reboot'
    $self = [guid]::NewGuid().ToString()
    Queue $self @($ctx.Client)
    Cycle
    Check ([IO.File]::Exists("$root/failed/$self.json")) 'Self delivery accepted'
    $script:peerOnline = $false
    $expired = [guid]::NewGuid().ToString()
    Queue $expired @($peerId)
    Cycle
    $rec = Journal $expired
    Check ($rec.targets[0].started -eq 0 -and $rec.targets[0].state -ceq 'pending') 'Offline draft lost before its deadline'
    $rec.queued -= 86401
    Write-ChatObject "$root/deliveries/$expired.json" $rec
    Cycle
    Check ((Journal $expired).targets[0].state -ceq 'failed' -and [IO.File]::Exists("$root/failed/$expired.json")) 'Offline draft never expired'
    $local = Join-Path $root 'session'
    $localId = [guid]::NewGuid().ToString('N').Substring(0,8).ToUpperInvariant()
    Import-Module Microsoft.PowerShell.Security -ErrorAction Stop
    $subject = "CN=Codex-LAN-Peer-v1-$localId"
    Check (-not (Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -ceq $subject)) 'Fixture ID already in use'
    $probe = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([IPAddress]::Loopback,0))
    $port = $probe.Client.LocalEndPoint.Port
    $probe.Dispose()
    $session = $null
    try {
        $session = Start-AgentChatSession $PSScriptRoot "$local/missing.json" $localId $local 'Fixture desk' @{
            BindAddress='127.0.0.1';DiscoveryAddress='127.0.0.2';DiscoveryPort=$port;TcpPort=0
        }
        $deadline = [datetime]::UtcNow.AddSeconds(25)
        do {
            foreach ($job in $session.Jobs) { Check (-not $job.Task.IsCompleted) 'Chat runspace exited early' }
            $live = Read-ChatObject "$local/roster.json"
            if ($live.self_fingerprint -cmatch '\A[0-9A-F]{64}\z') { break }
            Start-Sleep -Milliseconds 150
        } while ([datetime]::UtcNow -lt $deadline)
        Check ($live.self_fingerprint -cmatch '\A[0-9A-F]{64}\z' -and $live.agents.id -ccontains $localId) 'Peer runtime did not start'
    } finally {
        if ($session) { Stop-AgentChatSession $session }
        foreach ($cert in (Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -ceq $subject)) {
            Remove-Item -LiteralPath ("Cert:\CurrentUser\My\" + $cert.Thumbprint) -DeleteKey
        }
    }
    foreach ($job in $session.Jobs) {
        Check ($job.Space.RunspaceStateInfo.State -eq 'Closed' -and -not $job.Failure) "Failed to close $($job.Role)"
    }
    'PASS: independent delivery, mixed recipients, no idle HTTP, pinned roster, restart retry, legacy migration, board reboot and runspace lifecycle'
} finally {
    if ($proc) { if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }; $proc.Dispose() }
    if ([IO.Path]::GetDirectoryName($root) -eq [IO.Path]::GetTempPath().TrimEnd('\') -and [IO.Path]::GetFileName($root) -match '^chat-fixture-[0-9a-f-]{36}$') {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}
