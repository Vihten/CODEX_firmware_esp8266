$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$helper = Join-Path $PSScriptRoot 'weekly_tokens.ps1'
. $helper

function Check($Actual, $Expected, [string]$Label) {
    if ($Actual -cne $Expected) { throw "$Label expected=$Expected actual=$Actual" }
}

function New-Observation([string]$Id, [string]$At, [long]$Output = 3, [string]$Type = 'assistant') {
    return (@{ type = $Type; timestamp = $At; uuid = [guid]::NewGuid().ToString(); requestId = 'shared-request';
        message = @{ id = $Id; model = 'test-model'; usage = @{ input_tokens = 2; output_tokens = $Output;
            cache_creation_input_tokens = 5; cache_read_input_tokens = 7; cache_creation = @{ ephemeral_5m_input_tokens = 5 } } }
    } | ConvertTo-Json -Depth 8 -Compress)
}

function Wait-ClaudeScan([DateTimeOffset]$At, [string]$Identity = '12345678,87654321,ABCDEF,74657374') {
    Check (Start-WeeklyTokenScan $scanner $helper $projects $checkpoint $At $Identity -ClaudeDaily) $true 'Start Claude worker'
    Check (Start-WeeklyTokenScan $scanner $helper $projects $checkpoint $At $Identity -ClaudeDaily) $false 'Single pending Claude worker'
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        $result = Receive-WeeklyTokenScan $scanner
        if ($null -ne $result) { return $result }
        Start-Sleep -Milliseconds 20
    }
    throw 'Claude worker timed out'
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('claude-daily-' + [guid]::NewGuid().ToString('N'))
$scanner = New-WeeklyTokenScanner
try {
    $projects = Join-Path $root 'projects'
    $project = Join-Path $projects 'example'
    $subagents = Join-Path $project 'session/subagents'
    [void][IO.Directory]::CreateDirectory($subagents)
    $main = Join-Path $project 'session.jsonl'
    $fork = Join-Path $project 'fork.jsonl'
    $agent = Join-Path $subagents 'agent-test.jsonl'
    $checkpoint = Join-Path $root 'checkpoint.json'
    $beforeMidnight = '2026-07-05T20:59:00Z'
    $afterMidnight = '2026-07-05T21:01:00Z'
    $now = [DateTimeOffset]'2026-07-05T21:05:00Z'
    $first = New-Observation 'msg_First' $beforeMidnight
    $partial = New-Observation 'msg_partial' $afterMidnight
    $later = New-Observation 'msg_future' '2026-07-05T21:06:00Z'
    $old = New-Observation 'msg_old' '2025-05-01T09:00:00Z'
    $user = New-Observation 'msg_user' $afterMidnight -Type 'user'
    $synthetic = (New-Observation 'msg_synthetic' $afterMidnight).Replace('test-model', '<synthetic>')
    $malformed = (New-Observation 'msg_bad' $afterMidnight).Replace('"input_tokens":2', '"input_tokens":-1')
    $missing = (New-Observation 'msg_missing' $afterMidnight).Replace('msg_missing', '')
    $copy = New-Observation 'msg_First' $afterMidnight 13
    # Repeated API-message ID, different transcript UUID and timestamp. The
    # distinct case-sensitive ID below intentionally shares its request ID.
    [IO.File]::WriteAllText($main, ([char]0xFEFF + "$first`r`n$first`n$old`n$user`n$synthetic`n$malformed`n$missing`n{bad}`n$later`n$partial"))
    [IO.File]::WriteAllText($fork, "$copy`n")
    [IO.File]::WriteAllText($agent, ((New-Observation 'msg_first' $afterMidnight) + "`n"))

    $result = Wait-ClaudeScan $now
    Check $result.Daily.Count 2 'Two Kyiv days observed'
    Check $result.Daily[0].Day '20260706' 'Summer Kyiv midnight'
    Check $result.Daily[0].Tokens 17L 'Subagent counted, incomplete and future lines withheld'
    Check $result.Daily[1].Day '20260705' 'Fork attributed to first date'
    Check $result.Daily[1].Tokens 27L 'Largest full usage snapshot, cache fields counted once'
    $revision = $result.Daily[0].Revision
    $saved = Read-ClaudeDailyTokenState $checkpoint
    Check $saved.Days['20260706'].Revision $revision 'Worker returns durable revision'
    Check $saved.Evidence.Count 3 'Only valid responses retained, including future evidence'
    Check $saved.Files[$main].Offset ([IO.FileInfo]::new($main).Length - $partial.Length) 'Cursor stays before incomplete line'

    Stop-WeeklyTokenScanner $scanner
    $scanner = New-WeeklyTokenScanner
    [IO.File]::AppendAllText($main, "`n")
    $result = Wait-ClaudeScan $now.AddMinutes(2)
    Check $result.Daily[0].Tokens 51L 'Restart reads completed line and releases future evidence'
    Check ($result.Daily[0].Revision -gt $revision) $true 'Changed row advances revision'
    $revision = $result.Daily[0].Revision
    [IO.File]::SetLastWriteTimeUtc($checkpoint, [DateTime]::UtcNow.AddMinutes(-10))
    $checkpointWriteTime = [IO.File]::GetLastWriteTimeUtc($checkpoint)
    $result = Wait-ClaudeScan $now.AddMinutes(3)
    Check $result.Daily[0].Revision $revision 'Unchanged replay keeps revision'
    Check ([IO.File]::GetLastWriteTimeUtc($checkpoint)) $checkpointWriteTime 'Unchanged checkpoint avoids a disk write'
    $identity = '12345678,87654321,012345,74657374'
    $result = Wait-ClaudeScan $now.AddMinutes(3) $identity
    Check $result.ReportIdentity $identity 'Committed identity changes'
    Check ($result.Daily[0].Revision -gt $revision) $true 'Identity change advances revision'

    # Truncation/replacement starts from zero. Rotated evidence and dedup keys
    # survive deletion, including forks rediscovered under a different path.
    [IO.File]::WriteAllText($main, "$first`n" + (New-Observation 'msg_rewrite' $afterMidnight) + "`n")
    [IO.File]::Delete($agent)
    $result = Wait-ClaudeScan $now.AddMinutes(4)
    Check $result.Daily[0].Tokens 68L 'Replacement adds new usage and preserves observed history'
    Check $result.Daily[1].Tokens 27L 'Smaller rewrite copy does not add tokens'

    # A record can span multiple file slices. The scanner discards that line
    # and eventually reads the complete valid record after it.
    $large = Join-Path $project 'large.jsonl'
    [IO.File]::WriteAllText($large, ('x' * ($script:ClaudeTokenSliceBytes + 23)) + "`n" +
        (New-Observation 'msg_after_large' $afterMidnight) + "`n")
    $result = Wait-ClaudeScan $now.AddMinutes(5)
    Check $result.Daily[0].Tokens 68L 'One slice per transcript per scan'
    Check (Read-ClaudeDailyTokenState $checkpoint).Files[$large].Discard $true 'Oversized line discard survives restart'
    $result = Wait-ClaudeScan $now.AddMinutes(6)
    Check $result.Daily[0].Tokens 85L 'Valid record after oversized line counted'

    # An earlier copy discovered later moves the response to its original day.
    $lateCopy = Join-Path $project 'earlier.jsonl'
    [IO.File]::WriteAllText($lateCopy, ((New-Observation 'msg_after_large' '2026-07-04T12:00:00Z') + "`n"))
    $result = Wait-ClaudeScan $now.AddMinutes(7)
    Check $result.Daily[0].Tokens 68L 'Earlier copy corrects a later day downward'
    Check $result.Daily[2].Tokens 17L 'Earlier Kyiv day receives the response'
    [IO.File]::AppendAllText($lateCopy, (New-Observation 'msg_after_large' '2026-07-03T12:00:00Z') + "`n")
    $result = Wait-ClaudeScan $now.AddMinutes(8)
    Check $result.Daily[2].Tokens 0L 'A day that loses all usage emits a zero correction'
    Check $result.Daily[3].Tokens 17L 'Newest evidence places usage on the true earliest date'

    $overflow = Join-Path $project 'overflow.jsonl'
    $committedBytes = [IO.File]::ReadAllText($checkpoint)
    [IO.File]::WriteAllText($overflow, (New-Observation 'msg_overflow' $afterMidnight 9007199254740991L) + "`n")
    $failed = $false
    try { [void](Wait-ClaudeScan $now.AddMinutes(9)) } catch { $failed = $_.Exception.Message -match 'overflow' }
    Check $failed $true 'Overflow suspends the Claude snapshot'
    Check ([IO.File]::ReadAllText($checkpoint)) $committedBytes 'Overflow leaves last commit intact'
    [IO.File]::Delete($overflow)

    $result = Wait-ClaudeScan $now.AddDays(367)
    Check $result.Daily.Count 0 'Evict dates and dedup evidence outside 366 days'
    Check (Read-ClaudeDailyTokenState $checkpoint).Evidence.Count 0 'Old evidence pruned'

    [IO.File]::WriteAllText($checkpoint, '{"Version":1}')
    $failed = $false
    try { [void](Wait-ClaudeScan $now) } catch { $failed = $_.Exception.Message -match 'checkpoint invalid' }
    Check $failed $true 'Corrupt checkpoint publishes no replacement'
    Check ([IO.File]::ReadAllText($checkpoint)) '{"Version":1}' 'Original corrupt checkpoint preserved'
    [IO.File]::Delete($checkpoint)
    $result = Wait-ClaudeScan $now.AddMinutes(10)
    Check ($result.Daily.Count -gt 0) $true 'Worker recovers after checkpoint repair'

    # Small fixture budget exercises the rotating discovery cursor without a
    # real backlog. A busy early path cannot starve the next file.
    $budgetRoot = Join-Path $root 'budget'
    [void][IO.Directory]::CreateDirectory($budgetRoot)
    $a = Join-Path $budgetRoot 'a.jsonl'
    $b = Join-Path $budgetRoot 'b.jsonl'
    [IO.File]::WriteAllText($a, (New-Observation 'msg_a' $afterMidnight) + "`n")
    [IO.File]::WriteAllText($b, (New-Observation 'msg_b' $afterMidnight) + "`n")
    $state = Read-ClaudeDailyTokenState (Join-Path $root 'budget.json')
    $script:ClaudeTokenScanFiles = 1
    [void](Update-ClaudeDailyTokenState $state $budgetRoot $now $identity)
    Check $state.Evidence.Count 1 'File budget bounds the first pass'
    [IO.File]::AppendAllText($a, (New-Observation 'msg_a2' $afterMidnight) + "`n")
    [void](Update-ClaudeDailyTokenState $state $budgetRoot $now $identity)
    Check $state.LastPath $b 'Next scan advances past the first busy transcript'
    Check $state.Evidence.Count 2 'Second file processed within budget'
    Write-Host 'PASS Claude daily accounting, incremental checkpoints, fork and subagent dedup, Kyiv dates, bounded records and recovery.'
}
finally {
    Stop-WeeklyTokenScanner $scanner
    $resolved = [IO.Path]::GetFullPath($root)
    if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
