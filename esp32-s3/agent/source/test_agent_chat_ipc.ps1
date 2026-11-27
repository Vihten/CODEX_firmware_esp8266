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
    function Cycle { Invoke-ChatCycle $root $ep $ctx }
    Cycle
    $stamp = [IO.File]::GetLastWriteTimeUtc("$root/roster.json")
    Cycle
    Check ((Get-Content "$root/calls") -join ',' -ceq 'state,roster,inbox,inbox') 'Idle request budget exceeded'
    $ctx.NextRoster = [datetime]::MinValue
    Cycle
    Check ([IO.File]::GetLastWriteTimeUtc("$root/roster.json") -eq $stamp) 'Unchanged roster rewritten'
    $id = [guid]::NewGuid().ToString()
    Queue $id @('board','AABBCCDD')
    Mode uncertain
    Reject { Cycle }
    Check ([IO.File]::Exists("$root/attempts/$id.json")) 'Missing durable attempt'
    Mode idle
    Cycle
    Check (-not [IO.File]::Exists("$root/outbox/$id.json")) 'Send receipt ignored'
    Check (-not [IO.File]::Exists("$root/failed/$id.json")) 'Valid send failed'
    $id = [guid]::NewGuid().ToString()
    Queue $id @('board','AABBCCDD')
    Mode uncertain
    Reject { Cycle }
    Mode reboot
    Reject { Cycle }
    Cycle
    Check ([IO.File]::Exists("$root/failed/$id.json")) 'Replayed uncertain send after boot'
    Mode idle
    $ctx.NextState = [datetime]::MinValue
    $self = [guid]::NewGuid().ToString()
    Queue $self @('01020304')
    Cycle
    Check ([IO.File]::Exists("$root/failed/$self.json")) 'Self delivery accepted'
    Mode ackfail
    Reject { Cycle }
    $incoming = '12345678-1234-4123-8123-123456789abc.json'
    [IO.File]::Move("$root/inbox/$incoming","$root/seen/$incoming")
    Mode delivery
    Cycle
    Check (-not [IO.File]::Exists("$root/inbox/$incoming")) 'Duplicate delivery'
    foreach ($mode in 'unsigned','oversize','redirect') {
        Mode $mode
        Reject { Invoke-ChatRest $ctx inbox }
    }
    Mode idle
    # Separate synthetic client does not share the fixture server's sequence counter.
    # Exercise the production runspaces without an enrolled key or a live controller.
    $local = Join-Path $root 'session'
    $session = Start-AgentChatSession $PSScriptRoot (Join-Path $local 'missing.json') '05060708' $local
    try {
        Start-Sleep -Milliseconds 2200
        foreach ($job in $session.Jobs) { Check (-not $job.Task.IsCompleted) 'Chat runspace exited early' }
    } finally { Stop-AgentChatSession $session }
    foreach ($job in $session.Jobs) {
        Check ($job.Space.RunspaceStateInfo.State -eq 'Closed') 'Chat runspace not closed'
        Check (-not $job.Failure) "Runspace error during lifecycle ($($job.Role)): $($job.Failure)"
    }
    'PASS: signed REST, idle load, atomic delivery, retries, boot change, invalid responses'
} finally {
    if ($proc) { if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }; $proc.Dispose() }
    if ([IO.Path]::GetDirectoryName($root) -eq [IO.Path]::GetTempPath().TrimEnd('\') -and [IO.Path]::GetFileName($root) -match '^chat-fixture-[0-9a-f-]{36}$') {
        Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    }
}
