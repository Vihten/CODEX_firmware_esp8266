$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'weekly_tokens.ps1')
$script:dailyAssertions = 0
function Check($Actual, $Expected, [string]$Label) { ++$script:dailyAssertions; if ($Actual -cne $Expected) { throw "$Label expected=$Expected actual=$Actual" } }
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'codex_lan_agent.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach($name in @('Send-Heartbeat','Test-DailyTokenAcknowledgement','Test-WeeklyTokenAcknowledgement','Confirm-DirectController')) {
    $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if ($null -eq $definition) { throw "Missing $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$clientHash='87654321'; $controllerDeviceId='ABC123'; $AccountFingerprint='12345678'
$clientId='test'; $CollectorHost='127.0.0.1'; $Port=4212; $DirectHost='127.0.0.1'; $DirectPort=4210; $DirectHttpPort=80
$metadata=@{Name='test';ColorHex6='ABCDEF';NameUtf8Hex='74657374'}
$activity=0.1; $remainingPercent=80; $effectiveColorHex6='ABCDEF'
$dailyTokenIdentity='12345678,87654321,ABCDEF,74657374'
$dailyTokens=@(
    @{Day='20261105';Tokens=120L;Revision=100L},
    @{Day='20261104';Tokens=0L;Revision=101L},
    @{Day='20261103';Tokens=9L;Revision=102L})
$dailyTokenIndex=0; $weeklyTokensReady=$false; $weeklyTokens=$null
$nextDailyReportAt=[DateTime]::MinValue; $nextDirectHttpAt=[DateTime]::MaxValue; $nextWeeklyReportAt=[DateTime]::MaxValue
$claudeDailyTokens=$null; $preferClaudeDaily=$false
$nextOriginHttpAt=[DateTime]::MaxValue # This fixture isolates daily statistics; origin HTTP has its own tests.
$httpPackets=[Collections.Generic.List[string]]::new()
$udpPackets=[Collections.Generic.List[string]]::new()
$udp=New-Object PSObject
$udp | Add-Member ScriptMethod Send {
    param($Bytes,$Length,$HostAddress,$PortNumber)
    $script:udpPackets.Add([Text.Encoding]::ASCII.GetString($Bytes)); return $Length
}
$replyMode='valid'
function Send-DirectHttpPacket([byte[]]$Bytes) {
    $packet=[Text.Encoding]::ASCII.GetString($Bytes); $script:httpPackets.Add($packet)
    $p=$packet -split ','
    if($script:replyMode -eq 'failure') { return $null }
    if($script:replyMode -eq 'stale') { return "$($p[0])_RAM_ACK,ABC123,87654321,$($p[3]),1" }
    return "$($p[0])_RAM_ACK,ABC123,87654321,$($p[3]),$($p[5])"
}
$day=$dailyTokens[0]
Check (Test-DailyTokenAcknowledgement 'LFD1_RAM_ACK,ABC123,87654321,20261105,100' $day) $true 'Exact ACK'
foreach($ack in @('LFD1_RAM_ACK,ABC123,87654321,20261104,100','LFD1_RAM_ACK,ABC123,87654321,20261105,99',
    'LFD1_RAM_ACK,ABC123,87654321,20261105,101','LFD1_RAM_ACK,ABC123,00000000,20261105,100',
    'LFD1_RAM_ACK,FFFFFF,87654321,20261105,100','LFD1_RAM_ACK,ABC123,87654321',
    'LFD1_RAM_ACK,ABC123,87654321,20261105,100,extra','LFT2_ACK,ABC123,87654321,20261105,100')) {
    Check (Test-DailyTokenAcknowledgement $ack $day) $false 'Reject wrong day/revision/client/controller/protocol'
}
$controllerCacheDirty=$false; $lastDirectAckAt=[DateTime]::MinValue; $directConnected=$false; $directStatusKnown=$false
$confirmedDirectHost=$DirectHost
$before=@($effectiveColorHex6,$DirectHost,$controllerDeviceId,$lastDirectAckAt,$directConnected,$directStatusKnown,$controllerCacheDirty)|ConvertTo-Json
Confirm-DirectController 'LFD1_RAM_ACK,ABC123,87654321,20261105,100' 'mock' '127.0.0.2'
$after=@($effectiveColorHex6,$DirectHost,$controllerDeviceId,$lastDirectAckAt,$directConnected,$directStatusKnown,$controllerCacheDirty)|ConvertTo-Json
Check $after $before 'Daily ACK cannot adopt endpoint/color or authenticate heartbeat'
Send-Heartbeat
Check $httpPackets.Count 1 'Only one daily HTTP packet'
Check ($httpPackets[0] -clike 'LFD1,12345678,87654321,20261105,120,100,*') $true 'Newest day'
Check (($nextDailyReportAt-(Get-Date)).TotalSeconds -gt 1) $true 'Two second rate limit'
Send-Heartbeat
Check $httpPackets.Count 1 'No backlog burst inside interval'
$nextDailyReportAt=[DateTime]::MinValue; $replyMode='failure'; Send-Heartbeat
Check ($httpPackets[1] -clike 'LFD1,*,20261104,0,101,*') $true 'Zero correction sent'
$nextDailyReportAt=[DateTime]::MinValue; $replyMode='stale'; Send-Heartbeat
Check ($httpPackets[2] -clike 'LFD1,*,20261103,9,102,*') $true 'Next oldest bounded'
$nextDailyReportAt=[DateTime]::MinValue; $replyMode='valid'; Send-Heartbeat
Check $httpPackets[3] $httpPackets[0] 'Cyclic idempotent resend after loss/reboot'
Check @($udpPackets | Where-Object { $_ -clike 'LFD1,*' }).Count 4 'Same daily UDP retry count'
$dailyTokenIdentity='different'; $nextDailyReportAt=[DateTime]::MinValue; Send-Heartbeat
Check $httpPackets.Count 4 'Never send persisted stats under changed identity'
$dailyTokenIdentity='12345678,87654321,ABCDEF,74657374'
# Never stack a daily HTTP request on a scheduled weekly HTTP transfer.
$weeklyTokensReady=$true
$weeklyTokens=@{ReportIdentity=$dailyTokenIdentity;Week=(Get-TokenWeek ([DateTimeOffset]::UtcNow));Total=1L;LifetimeTotal=2L;Revision=200L}
$nextWeeklyReportAt=[DateTime]::MinValue; $nextDailyReportAt=[DateTime]::MinValue
Send-Heartbeat
Check $httpPackets.Count 5 'Single HTTP token transfer when weekly due'
Check ($httpPackets[4] -clike 'LFT2,*') $true 'Weekly existing reporting preserved'
# Verify weekly and daily are published from one durable checkpoint.
$weeklyTokensReady=$false
$dailyTokenIndex=0
$cycleStart=$httpPackets.Count
for($i=0;$i -lt 6;++$i) {
    $nextDailyReportAt=[DateTime]::MinValue
    Send-Heartbeat
    Check $httpPackets[$cycleStart+$i] $httpPackets[$i%3] 'All RAM-ACKed dates replay cyclically'
}
$claudeDay = @{Day='20261105';Tokens=321L;Revision=500L}
$claudeDailyTokens = @{Daily=@($claudeDay, @{Day='20261104';Tokens=0L;Revision=501L});ReportIdentity=$dailyTokenIdentity;Index=0}
$preferClaudeDaily=$false
$dailyTokenIndex=0
$lfd2='LFD2,12345678,87654321,20261105,321,500,ABCDEF,74657374'
$lfd2Zero='LFD2,12345678,87654321,20261104,0,501,ABCDEF,74657374'
$ack='LFD2_RAM_ACK,ABC123,87654321,20261105,500'
Check (Test-DailyTokenAcknowledgement $ack $claudeDay LFD2) $true 'Claude exact ACK'
Check (Test-DailyTokenAcknowledgement $ack $claudeDay) $false 'Claude ACK cannot acknowledge Codex'
foreach ($invalid in @($ack.Replace('LFD2', 'LFD1'), $ack.Replace('500', '499'), $ack.Replace('20261105', '20261104'),
    $ack.Replace('87654321', '00000000'), $ack.Replace('ABC123', 'ABC124'))) {
    Check (Test-DailyTokenAcknowledgement $invalid $claudeDay LFD2) $false 'Reject invalid Claude ACK'
}
Confirm-DirectController $ack 'mock' '127.0.0.2'
Check (@($effectiveColorHex6,$DirectHost,$controllerDeviceId,$lastDirectAckAt,$directConnected,$directStatusKnown,$controllerCacheDirty)|ConvertTo-Json) $before 'Claude ACK cannot adopt endpoint/color'
$mixedStart=$httpPackets.Count
$expected=@($httpPackets[0],$lfd2,$httpPackets[1],$lfd2Zero,$httpPackets[2],$lfd2)
for ($i=0; $i -lt $expected.Count; ++$i) {
    $nextDailyReportAt=[DateTime]::MinValue
    Send-Heartbeat
    Send-Heartbeat
    Check $httpPackets.Count ($mixedStart+$i+1) 'Providers share one HTTP slot every two seconds'
    Check $httpPackets[$mixedStart+$i] $expected[$i] 'Fair provider rotation with separate cyclic backlogs'
}
Check (@($udpPackets | Where-Object { $_ -clike 'LFD2,*' }) -join '|') "$lfd2|$lfd2Zero|$lfd2" 'Claude UDP and HTTP match byte for byte'
$dailyTokenIdentity='different'
$nextDailyReportAt=[DateTime]::MinValue; $replyMode='failure'; Send-Heartbeat
Check $httpPackets[$httpPackets.Count-1] $lfd2Zero 'Claude-only transport sends after Codex identity mismatch'
$claudeDailyTokens.ReportIdentity='different'
$mixedEnd=$httpPackets.Count
$nextDailyReportAt=[DateTime]::MinValue; Send-Heartbeat
Check $httpPackets.Count $mixedEnd 'Never send Claude rows under changed identity'
$dailyTokenIdentity='12345678,87654321,ABCDEF,74657374'
$root=Join-Path ([IO.Path]::GetTempPath()) ('daily-http-'+[guid]::NewGuid().ToString('N'))
$scanner=$null
try {
    [void](New-Item -ItemType Directory -Path $root)
    $sessions=Join-Path $root 'sessions'; [void](New-Item -ItemType Directory -Path $sessions)
    $at=[DateTimeOffset]::UtcNow.AddMinutes(-1).ToString('o')
    $a='{"timestamp":"'+$at+'","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1000,"output_tokens":100}}}}'
    $b='{"timestamp":"'+$at+'","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1010,"output_tokens":102}}}}'
    [IO.File]::WriteAllText((Join-Path $sessions 'one.jsonl'),"$a`n$b`n")
    $checkpoint=Join-Path $root 'weekly.json'
    $scanner=New-WeeklyTokenScanner
    Check (Start-WeeklyTokenScan $scanner (Join-Path $PSScriptRoot 'weekly_tokens.ps1') $sessions $checkpoint -ReportIdentity $dailyTokenIdentity) $true 'Start background scan'
    $deadline=[DateTime]::UtcNow.AddSeconds(20); $committed=$null
    while([DateTime]::UtcNow -lt $deadline -and $null -eq $committed) {
        try { $committed=Receive-WeeklyTokenScan $scanner } catch { $scanner.Worker.Streams.Error | Out-String | Write-Host; throw }
        if($null -eq $committed) { Start-Sleep -Milliseconds 20 }
    }
    if($null -eq $committed) { throw 'Background daily commit timed out' }
    Check $committed.DailyError '' 'No hidden daily scanner failure'
    Check (Test-Path -LiteralPath $checkpoint) $true 'Shared durable checkpoint before snapshot return'
    Check @($committed.Daily).Count 1 'One observed day, not zero-filled year'
    Check $committed.Daily[0].Tokens 12L 'Daily excludes cumulative baseline'
    Check $committed.Total 12L 'Weekly uses the same dated evidence'
    Check $committed.LifetimeTotal 1112L 'Lifetime cumulative history preserved'
    Check $committed.ReportIdentity $dailyTokenIdentity 'Committed identity'
    $saved=(Read-WeeklyTokenState $checkpoint $committed.Week).DailyLedger
    Check $saved.Days[$committed.Daily[0].Day].Revision $committed.Daily[0].Revision 'Wire revision matches disk'
    # The obsolete sidecar cannot override the embedded, authoritative ledger.
    [IO.File]::WriteAllText("$checkpoint.daily-v1.json", 'corrupt obsolete sidecar')
    Check (Start-WeeklyTokenScan $scanner (Join-Path $PSScriptRoot 'weekly_tokens.ps1') $sessions $checkpoint -ReportIdentity $dailyTokenIdentity) $true 'Retry scan'
    $committed=$null; $deadline=[DateTime]::UtcNow.AddSeconds(20)
    while([DateTime]::UtcNow -lt $deadline -and $null -eq $committed) {
        try { $committed=Receive-WeeklyTokenScan $scanner } catch { $scanner.Worker.Streams.Error | Out-String | Write-Host; throw }
        if($null -eq $committed) { Start-Sleep -Milliseconds 20 }
    }
    Check ($null -ne $committed) $true 'Shared snapshot ignores obsolete sidecar'
    Check $committed.Daily[0].Tokens 12L 'Embedded evidence remains authoritative'
    Check $committed.DailyError '' 'No separate daily commit'
    # Overflow fails the entire shared snapshot. Each fixture is MAXSAFE+1.
    foreach($kind in @('delta','total')) {
        $overflowRoot=Join-Path $root $kind
        [void](New-Item -ItemType Directory -Path $overflowRoot)
        $usage=@{input_tokens=9007199254740991L;output_tokens=$(if($kind -eq 'delta'){1L}else{0L})}
        $record=@{timestamp=$at;type='event_msg';payload=@{type='token_count';info=@{
            total_token_usage=$usage;last_token_usage=$usage}}} | ConvertTo-Json -Compress -Depth 8
        [IO.File]::WriteAllText((Join-Path $overflowRoot 'first.jsonl'),"$record`n")
        if($kind -eq 'total') {
            $usage=@{input_tokens=1L;output_tokens=0L}
            $record=@{timestamp=$at;type='event_msg';payload=@{type='token_count';info=@{
                total_token_usage=$usage;last_token_usage=$usage}}} | ConvertTo-Json -Compress -Depth 8
            [IO.File]::WriteAllText((Join-Path $overflowRoot 'second.jsonl'),"$record`n")
        }
        $overflowCheckpoint=Join-Path $root "$kind-weekly.json"
        Check (Start-WeeklyTokenScan $scanner (Join-Path $PSScriptRoot 'weekly_tokens.ps1') $overflowRoot $overflowCheckpoint -ReportIdentity $dailyTokenIdentity) $true "$kind overflow scan starts"
        $committed=$null; $failed=$false; $deadline=[DateTime]::UtcNow.AddSeconds(20)
        while([DateTime]::UtcNow -lt $deadline -and $null -ne $scanner.Pending) {
            try { $committed=Receive-WeeklyTokenScan $scanner } catch { $failed=$true }
            if($null -ne $scanner.Pending) { Start-Sleep -Milliseconds 20 }
        }
        Check $failed $true "$kind overflow fails explicitly"
        Check ($null -eq $committed) $true "$kind overflow suspends shared publication"
        Check (Test-Path -LiteralPath $overflowCheckpoint) $false "$kind overflow never commits shared state"
        Check (Test-Path -LiteralPath "$overflowCheckpoint.daily-v1.json") $false "$kind overflow never commits daily state"
        Check (Test-Path -LiteralPath "$overflowCheckpoint.daily-v1.json.$PID.tmp") $false "$kind overflow never stages daily state"
    }
    Write-Host "PASS daily HTTP/backlog: $script:dailyAssertions assertions; exact date/revision ACK, no identity side effects, bounded one packet, cyclic retry, zero correction, shared weekly/daily commit, failure gating, delta/total overflow rejection."
} finally {
    Stop-WeeklyTokenScanner $scanner
    $resolved=[IO.Path]::GetFullPath($root)
    if($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
