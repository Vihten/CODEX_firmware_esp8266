# Only the bridge carries this transaction. The original supervisor owns the
# mutex, worker stop, heartbeat deadline, and restart handoff.
function Get-AgentBridgeLegacyFiles {
    Get-AgentCodeFiles | Where-Object { $_ -notin @('agent_chat_ipc.ps1','agent_chat_ui.ps1') }
}

function Install-AgentStagedUpdate([string]$Root, [string]$Stage) {
    $journal = Join-Path $Root '.agent-update-journal.json'
    Assert-AgentPlainPath $journal
    if ([IO.File]::Exists($journal)) { throw 'Recovery required' }
    $installed = Get-AgentInstalledVersion $Root
    $package = Test-AgentPackage $Root $Stage $installed
    if ($installed -ne 20261126000000 -or $package.Version -lt 20261127120000) { throw 'Unexpected bridge transition' }

    $previous = Join-Path $Root '.agent-bridge-previous'
    $forward = Join-Path $Root '.agent-update-backup'
    foreach ($dir in @($previous, $forward)) {
        Assert-AgentPlainPath $dir
        [void][IO.Directory]::CreateDirectory($dir)
    }
    $missing = 0
    foreach ($name in Get-AgentCodeFiles) {
        $path = Join-Path $Root $name
        Assert-AgentPlainPath $path
        if ([IO.File]::Exists($path)) {
            Write-AgentDurable (Join-Path $previous $name) ([IO.File]::ReadAllBytes($path))
        } elseif ([IO.Directory]::Exists($path)) { throw "Expected file: $name" }
        elseif ($name -eq 'agent_chat_ipc.ps1') { $missing += 1 }
        elseif ($name -eq 'agent_chat_ui.ps1') { $missing += 2 }
        else { throw "Missing baseline: $name" }
        # The immutable final helper can replay this complete ten-file image.
        Write-AgentDurable (Join-Path $forward $name) $package.Files[$name]
    }
    Write-AgentDurable $journal ([Text.Encoding]::ASCII.GetBytes("bridge-pending-$missing"))
    try {
        foreach ($name in Get-AgentCodeFiles) {
            if ($name -in @('agent_update.ps1','start_agent_supervisor.ps1')) { continue }
            Write-AgentDurable (Join-Path $Root $name) $package.Files[$name]
        }
    } catch { Restore-AgentUpdate $Root; throw }
}

function Finish-AgentBridgeCommit([string]$Root) {
    # The provisional worker has passed health. Activate the supervisor first.
    # Once the final helper is visible, its stock journal replay is sufficient.
    foreach ($name in @('start_agent_supervisor.ps1','agent_update.ps1')) {
        $image = Join-Path $Root ".agent-update-backup/$name"
        Assert-AgentPlainPath $image
        Write-AgentDurable (Join-Path $Root $name) ([IO.File]::ReadAllBytes($image))
    }
    [IO.File]::Delete((Join-Path $Root '.agent-update-journal.json'))
}

function Complete-AgentUpdate([string]$Root) {
    $journal = Join-Path $Root '.agent-update-journal.json'
    Assert-AgentPlainPath $journal
    if ([IO.File]::ReadAllText($journal) -cnotmatch '\Abridge-pending-[0-3]\z') { throw 'Unexpected health transaction' }
    Write-AgentDurable $journal ([Text.Encoding]::ASCII.GetBytes('bridge-healthy'))
    Finish-AgentBridgeCommit $Root
}

function Restore-AgentUpdate([string]$Root) {
    $journal = Join-Path $Root '.agent-update-journal.json'
    Assert-AgentPlainPath $journal
    if (-not [IO.File]::Exists($journal)) { return }
    $state = [IO.File]::ReadAllText($journal)
    if ($state -ceq 'bridge-healthy') {
        Finish-AgentBridgeCommit $Root
    } else {
        $missing = 0
        if ($state -ceq '{"pending":true}') {
            # Old supervisor installed the bridge and died before health commit.
            $names = Get-AgentBridgeLegacyFiles
            $dir = '.agent-update-backup'
        } elseif ($state -cmatch '\Abridge-pending-([0-3])\z') {
            $missing = [int]$Matches[1]
            $names = Get-AgentCodeFiles
            $dir = '.agent-bridge-previous'
        } else { throw 'Unknown bridge journal' }
        $prior = @{}
        foreach ($name in $names) {
            Assert-AgentPlainPath (Join-Path $Root $name)
            if (($name -eq 'agent_chat_ipc.ps1' -and ($missing -band 1)) -or
                ($name -eq 'agent_chat_ui.ps1' -and ($missing -band 2))) { continue }
            $path = Join-Path $Root "$dir/$name"
            Assert-AgentPlainPath $path
            $prior[$name] = [IO.File]::ReadAllBytes($path)
        }
        foreach ($name in $names) {
            $path = Join-Path $Root $name
            if ($prior.ContainsKey($name)) { Write-AgentDurable $path $prior[$name] }
            else { [IO.File]::Delete($path) }
        }
        [IO.File]::Delete($journal)
    }
    # Cold startup may have already loaded the provisional helper or previous
    # supervisor. Use the supervisor's ordinary mutex-safe restart handoff.
    if ($MyInvocation.ScriptName -and
        [IO.Path]::GetFileName($MyInvocation.ScriptName) -eq 'start_agent_supervisor.ps1' -and
        -not $pendingHealth) { Set-Variable -Scope 1 -Name reloadRequired -Value $true }
}
