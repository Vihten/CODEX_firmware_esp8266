# Drive the real supervisor loop with an owned-process double and a monotonic test clock.
param([ValidateSet('Exited','Running','Backoff')][string]$StopMode = 'Exited')
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('supervisor-stall-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
try {
    $source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'start_agent_supervisor.ps1'))
    $source = $source.Replace('[Diagnostics.Stopwatch]::StartNew()', '$fixture.Clock')
    [IO.File]::WriteAllText("$root/start_agent_supervisor.ps1", $source)
    [IO.File]::WriteAllText("$root/codex_lan_agent.ps1", '')
    [IO.File]::WriteAllText("$root/agent_update.ps1", @"
function Repair-AgentGuiRecoveryTask {}
function Test-AgentUpdateReady { return (`$fixture.Launches -le 1) }
function Open-AgentManagementGuard { [IO.MemoryStream]::new() }
function Read-AgentActionState {}
function Invoke-AgentPendingAction {}
function Start-AgentUpdateDownloader {
    `$poll = [pscustomobject]@{ ExitCode = 0 }
    `$poll | Add-Member ScriptProperty HasExited { `$fixture.Clock.Elapsed.TotalSeconds -ge 2700 }
    `$poll | Add-Member ScriptMethod WaitForExit {}
    `$poll | Add-Member ScriptMethod Dispose { `$fixture.PollDone = `$true }
    return `$poll
}
"@)
    $fixture = @{ Launches = 0; Clock = @{ Elapsed = @{ TotalSeconds = 0 } }
        Alive = $false; PollDone = $false; Stops = @(); Sleeps = @(); Kills = 0 }
    function Start-Process {
        param($FilePath,$ArgumentList,$WorkingDirectory,$WindowStyle,$RedirectStandardOutput,$RedirectStandardError,[switch]$PassThru)
        if ($fixture.Alive -or $ArgumentList -notmatch '-UpdateControlPath "([^"]+)"') { throw 'Invalid replacement' }
        $fixture.Control = $Matches[1]
        $fixture.Launches++
        $fixture.Alive = $true
        $fixture.Clock.Elapsed.TotalSeconds = 0
        $proc = [pscustomobject]@{ Control = $Matches[1]; HasExited = $false; ExitCode = 0 }
        $proc | Add-Member ScriptMethod WaitForExit {
            param($ms)
            if ($this.HasExited) { return $true }
            if ($ms -eq 15000) {
                if (![IO.File]::Exists("$($this.Control).stop")) { throw 'Missing stop request' }
                if ($fixture.Launches -eq 8) {
                    $this.HasExited = $true
                    return $true
                }
                $fixture.Stops += $fixture.Clock.Elapsed.TotalSeconds
                return $false
            }
            $clock = $fixture.Clock.Elapsed
            $clock.TotalSeconds += 5
            # A long scan followed by sleep. The poll stays active past the new deadline.
            if ($fixture.Launches -eq 1 -and $clock.TotalSeconds -eq 290) { $clock.TotalSeconds = 2300 }
            # No poll on this launch: resume must independently grant a full grace period.
            if ($fixture.Launches -eq 2 -and $clock.TotalSeconds -eq 290) { $clock.TotalSeconds = 2300 }
            if (($fixture.Launches -eq 3 -and $clock.TotalSeconds -eq 295) -or
                ($fixture.Launches -eq 7 -and $clock.TotalSeconds -le 310)) {
                $path = "$($this.Control).health"
                [IO.File]::WriteAllText($path,'alive')
                [IO.File]::SetLastWriteTimeUtc($path, [datetime]::UtcNow.Date.AddSeconds($clock.TotalSeconds))
            }
            if ($fixture.Launches -eq 8) {
                [IO.File]::WriteAllText("$($this.Control).stop",'')
                $this.HasExited = $StopMode -eq 'Exited'
            }
            return $this.HasExited
        }
        $proc | Add-Member ScriptMethod Kill { $fixture.Kills++; $this.HasExited = $true }
        $proc | Add-Member ScriptMethod Refresh {}
        $proc | Add-Member ScriptMethod Dispose { $fixture.Alive = $false }
        return $proc
    }
    function Start-Sleep($Seconds) {
        $fixture.Sleeps += $Seconds
        if ($StopMode -eq 'Backoff' -and $fixture.Sleeps.Count -eq 7) {
            [IO.File]::WriteAllText("$($fixture.Control).stop",'')
        }
        if ($fixture.Sleeps.Count -ge 8) { throw 'Unexpected retry' }
    }
    & "$root/start_agent_supervisor.ps1" -InstanceName ('StallTest_' + [guid]::NewGuid().ToString('N'))
    $expectedLaunches = if ($StopMode -eq 'Backoff') { 7 } else { 8 }
    if (!$fixture.PollDone -or $fixture.Alive -or $fixture.Launches -ne $expectedLaunches -or $fixture.Kills -ne 7 -or
        ($fixture.Sleeps -join ',') -ne '10,20,40,80,120,120,10' -or
        ($fixture.Stops -join ',') -ne '2700,2600,595,300,300,300,610') {
        throw "Unexpected worker lifecycle: $($fixture | ConvertTo-Json -Depth 4)"
    }
    Write-Host 'PASS: stalls, missing heartbeat, suspension, poll exclusion, healthy reset, bounded retry and explicit stop.'
} finally {
    $target = (Resolve-Path -LiteralPath $root).Path
    if ([IO.Path]::GetDirectoryName($target) -ne [IO.Path]::GetTempPath().TrimEnd('\')) { throw 'Unsafe fixture path' }
    Remove-Item -LiteralPath $target -Recurse -Force
}
