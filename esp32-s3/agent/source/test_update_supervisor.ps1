param([ValidateSet('Failed', 'Healthy', 'LateEnrollment', 'NoIdentity', 'StopTimeout')][string]$Scenario = 'Failed')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'agent_update.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('lfau-supervisor-' + [guid]::NewGuid().ToString('N'))
$mockState = [pscustomobject]@{ Launches = 0; Sleeps = 0; Downloaded = $false; Reloaded = $false; Clock = [DateTime]::UtcNow; PollTimes = @(); Slices = 0 }
try {
    [void][IO.Directory]::CreateDirectory($testRoot)
    foreach ($name in Get-AgentCodeFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $testRoot }
    foreach ($name in @('agent-update-public.xml', 'agent-update-identity.json', 'controller-endpoint.json')) { [IO.File]::WriteAllText((Join-Path $testRoot $name), '{}') }
    if ($Scenario -in @('LateEnrollment', 'NoIdentity')) { [IO.File]::Delete((Join-Path $testRoot 'agent-update-identity.json')) }
    # Replace verification/install with deterministic in-memory faults; no package is executed.
    $helper = Join-Path $testRoot 'agent_update.ps1'
    $extra = @'
function Test-AgentPackage { [pscustomobject]@{ Version = 2 } }
function Install-AgentStagedUpdate([string]$Root, [string]$Stage) {
    if (-not $child.HasExited) { throw 'Installed while worker was running' }
    Write-AgentDurable (Join-Path $Root '.agent-update-journal.json') ([Text.Encoding]::ASCII.GetBytes('{"pending":true}'))
    [IO.File]::WriteAllText((Join-Path $Root 'installed-test-marker'), 'applied')
}
function Restore-AgentUpdate([string]$Root) {
    [IO.File]::WriteAllText((Join-Path $Root 'restored-test-marker'), 'restored')
    [IO.File]::Delete((Join-Path $Root '.agent-update-journal.json'))
}
'@
    # Test fixture generation, never a runnable upgrade payload.
    [IO.File]::AppendAllText($helper, "`n$extra")
    # Lifecycle tests mock only the background-poll entry point. The real
    # runspace/result/cancellation adapter is covered by test_supervisor_downloader.
    $supervisorPath = Join-Path $testRoot 'start_agent_supervisor.ps1'
    $supervisorSource = [IO.File]::ReadAllText($supervisorPath)
    $parseTokens = $null; $parseErrors = $null
    $supervisorAst = [Management.Automation.Language.Parser]::ParseInput($supervisorSource, [ref]$parseTokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
    $pollHelper = $supervisorAst.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Start-AgentUpdateDownloader'
    }, $true)
    if ($null -eq $pollHelper) { throw 'Missing background poll entry point' }
    $mockHelper = 'function Start-AgentUpdateDownloader([string]$Root) { Start-Process -FilePath "mock-only" -ArgumentList "agent_update_download.ps1" -WorkingDirectory $Root -WindowStyle Hidden -PassThru }'
    $supervisorSource = $supervisorSource.Replace($pollHelper.Extent.Text, $mockHelper)
    if ($Scenario -eq 'StopTimeout') { $supervisorSource = $supervisorSource.Replace('[DateTime]::UtcNow', '$mockState.Clock') }
    [IO.File]::WriteAllText($supervisorPath, $supervisorSource)
    function Start-Process {
        param($FilePath, $ArgumentList, $WorkingDirectory, $WindowStyle, $RedirectStandardOutput, $RedirectStandardError, [switch]$PassThru)
        $object = [pscustomobject]@{ HasExited = $false; ExitCode = 1; Kind = 'worker'; Calls = 0; Control = '' }
        if ($ArgumentList -like '*agent_update_restart.ps1*') {
            $mockState.Reloaded = $true
            if ($ArgumentList -notmatch '-ParametersBase64 ([A-Za-z0-9+/=]+)') { throw 'Missing typed reload parameters' }
            $parameters = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Matches[1])) | ConvertFrom-Json
            $restartArguments = Get-AgentSupervisorArguments $parameters $testRoot
            if ($restartArguments -notlike '*start_agent_supervisor.ps1*') { throw 'Wrong reload target' }
            $object.Kind = 'restart'
            return ($object | Add-Member ScriptMethod WaitForExit { param($Milliseconds) $false } -PassThru | Add-Member ScriptMethod Dispose { } -PassThru)
        }
        if ($ArgumentList -like '*agent_update_download.ps1*') {
            if ($Scenario -eq 'NoIdentity' -and (Test-Path -LiteralPath (Join-Path $testRoot 'agent-update-identity.json'))) { throw 'Downloader must start before enrollment' }
            $mockState.Downloaded = $true
            $mockState.PollTimes += $mockState.Clock
            $object.Kind = 'download'; $object.HasExited = $true; $object.ExitCode = 42
        } else {
            $mockState.Launches++
            if ($mockState.Launches -eq 2) {
                if ($Scenario -ne 'Healthy') { $object.Kind = 'failed-worker'; $object.HasExited = $true }
                else {
                    $object.Kind = 'healthy-worker'
                    if ($ArgumentList -notmatch '-UpdateControlPath "([^"]+)"') { throw 'Missing worker control path' }
                    $object.Control = $Matches[1]
                }
            }
            if ($mockState.Launches -gt 2) { throw 'Unexpected third launch' }
        }
        $object | Add-Member ScriptMethod WaitForExit {
            param($Milliseconds)
            $this.Calls++
            if ($this.Kind -eq 'download') { return $true }
            if ($this.Kind -eq 'failed-worker') { return $true }
            if ($Scenario -eq 'LateEnrollment' -and $this.Kind -eq 'worker') { [IO.File]::WriteAllText((Join-Path $testRoot 'agent-update-identity.json'), '{}') }
            if ($this.Kind -eq 'healthy-worker') {
                if ($Milliseconds -eq 15000) { $this.HasExited = $true; return $true }
                if (-not (Test-Path -LiteralPath (Join-Path $testRoot '.agent-update-journal.json'))) { $this.HasExited = $true; return $true }
                [IO.File]::WriteAllText(($this.Control + '.health'), 'mock heartbeat')
                [Threading.Thread]::Sleep(250)
                if ($this.Calls -gt 100) { throw 'Supervisor failed to commit healthy worker' }
                return $false
            }
            if ($Scenario -eq 'StopTimeout') {
                if ($Milliseconds -eq 15000) {
                    if (-not [IO.File]::Exists("$control.stop")) { throw 'Missing graceful stop request' }
                    $competingPoll = $null
                    try { $competingPoll = Open-AgentPollLock $testRoot } catch [IO.IOException] {}
                    if ($competingPoll) { $competingPoll.Dispose(); throw 'Management poll admitted during install stop' }
                    $mockState.Slices++
                    if ($mockState.Slices -lt 12) { return $false }
                } elseif ($Milliseconds -eq 250) {
                    if ([IO.File]::Exists("$control.stop") -or [IO.File]::Exists("$testRoot/.agent-update-failed-version.txt")) {
                        throw 'Timeout left a stop or failure marker'
                    }
                    $mockState.Clock = $mockState.Clock.AddSeconds(1)
                    if ($this.Calls -gt 500) { throw 'Same release never retried' }
                    return $false
                }
            }
            if ($Milliseconds -eq 15000) { $this.HasExited = $true; $this.ExitCode = 0; return $true }
            if ($this.Calls -gt 20) { throw 'Supervisor failed to stage update' }
            return $false
        }
        $object | Add-Member ScriptMethod Refresh { }
        $object | Add-Member ScriptMethod Dispose { }
        $object | Add-Member ScriptMethod Kill { $this.HasExited = $true }
        $object
    }
    function Start-Sleep {
        param($Seconds)
        $mockState.Sleeps++
        if ($mockState.Sleeps -ge 2) { throw 'TEST-END' }
    }
    $ended = $false
    try { & (Join-Path $testRoot 'start_agent_supervisor.ps1') -InstanceName ('TestUpdate_' + [guid]::NewGuid().ToString('N')) }
    catch { if ($_.Exception.Message -eq 'TEST-END') { $ended = $true } else { throw } }
    if (($Scenario -ne 'Healthy' -and -not $ended) -or ($Scenario -eq 'Healthy' -and -not $mockState.Reloaded) -or $mockState.Launches -ne 2 -or -not $mockState.Downloaded -or
        -not (Test-Path -LiteralPath (Join-Path $testRoot 'installed-test-marker')) -or
        ((Test-Path -LiteralPath (Join-Path $testRoot 'restored-test-marker')) -ne ($Scenario -ne 'Healthy')) -or
        (Test-Path -LiteralPath (Join-Path $testRoot '.agent-update-journal.json'))) {
        $detail = [IO.File]::ReadAllText((Join-Path $testRoot 'agent.log'))
        throw "Supervisor failed-worker rollback failed: ended=$ended state=$($mockState | ConvertTo-Json -Compress); $detail"
    }
    if ($Scenario -eq 'StopTimeout') {
        $expected = @(15,30,60,120,120)
        if ($mockState.PollTimes.Count -ne 6 -or $mockState.Slices -ne 12) { throw 'Unexpected retry or wait count' }
        for ($i=0; $i -lt $expected.Count; $i++) {
            # One simulated second elapses between downloader launch and its completion.
            $gap = ($mockState.PollTimes[$i+1] - $mockState.PollTimes[$i]).TotalSeconds
            if ($gap -ne $expected[$i]+1) { throw "Unexpected retry interval $gap" }
        }
        if (@(Select-String -LiteralPath "$testRoot/agent.log" -SimpleMatch 'deferred: worker still running').Count -ne 4) {
            throw 'Timeout diagnostics were missing or repeated at the cap'
        }
    }
    Write-Host "PASS: mocked supervisor $Scenario lifecycle, background staging, graceful stop, health commit/rollback. No worker/downloader process launched."
} finally {
    $full = [IO.Path]::GetFullPath($testRoot)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $full.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe cleanup' }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
}
