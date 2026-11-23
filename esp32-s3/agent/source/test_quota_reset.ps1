$ErrorActionPreference = 'Stop'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'codex_lan_agent.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in @('Get-AccountFingerprint','Get-RemainingPercent','New-QuotaResetPacket','ConvertTo-ClaudeQuota')) {
    $node=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    . ([scriptblock]::Create($node.Extent.Text))
}
function Assert($Value,$Message) { if (-not $Value) { throw $Message } }
$now=[DateTimeOffset]::FromUnixTimeSeconds(1800000000)
$marketing=Get-AccountFingerprint 'marketing@tiras.ua'
$q=@{Fingerprint=$marketing; ResetEpoch=1800003600; FetchedAt=$now}
$prefix="LFR1,53197668,12345678,codex,$marketing"
Assert ((New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now) -ceq "$prefix,1800003600,0") 'Seven-field schema'
Assert ((New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now.AddSeconds(65.9)) -ceq "$prefix,1800003600,65") 'Observation age increases; floors fractional seconds'
foreach ($value in @(1700000000,4102444800)) {
    $q.ResetEpoch=$value
    Assert ((New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now) -ceq "$prefix,$value,0") 'Inclusive epoch bounds'
}
foreach ($value in @($null,0,1699999999,4102444801,-1,'1800000000.0','1.8e9',$true,'999999999999999999999','1800000000,0')) {
    $q.ResetEpoch=$value
    Assert ((New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now) -ceq "$prefix,0,0") 'Invalid reset clears previous value'
}
$q.ResetEpoch=1800003600
Assert ((New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now.AddSeconds(86400)) -like '*,86400') 'Inclusive age bound'
foreach ($instant in @($now.AddSeconds(-0.1),$now.AddSeconds(86400.1))) {
    Assert ($null -eq (New-QuotaResetPacket '53197668' '12345678' 'codex' $q $instant)) 'Future/stale observation omitted'
}
$q.Fingerprint='00000000'
Assert ($null -eq (New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now)) 'Unknown identity omitted'
$q.Fingerprint=Get-AccountFingerprint 'other@example.invalid'
Assert ($null -eq (New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now)) 'Nonmarketing identity omitted'
$q.Fingerprint=$marketing
foreach ($provider in @('Codex','other','codex,claude')) {
    Assert ($null -eq (New-QuotaResetPacket '53197668' '12345678' $provider $q $now)) 'Provider strict'
}
Assert ($null -eq (New-QuotaResetPacket 'bad' '12345678' 'codex' $q $now)) 'Bad routing identity omitted'
$savedFetchedAt=$q.FetchedAt
$q.FetchedAt='2027-01-01'
Assert ($null -eq (New-QuotaResetPacket '53197668' '12345678' 'codex' $q $now)) 'Untyped observation rejected'
$q.FetchedAt=$savedFetchedAt
Assert ((New-QuotaResetPacket '53197668' '12345678' 'claude' $q $now) -ceq "LFR1,$marketing,12345678,claude,$marketing,0,0") 'Missing Claude reset clears previous value'
$q.ResetAt=$now.AddHours(1)
Assert ((New-QuotaResetPacket '53197668' '12345678' 'claude' $q $now.AddSeconds(7)) -ceq "LFR1,53197668,12345678,claude,$marketing,1800003600,7") 'Claude actual ResetAt'
$unused=ConvertTo-ClaudeQuota @{five_hour=@{utilization=0;resets_at=$null}} $now
Assert ($null -ne $unused -and $unused.Remaining -eq 100 -and $null -eq $unused.ResetAt) 'Unused Claude quota has no invented reset'
$unusedObservation=@{Fingerprint=$marketing;Remaining=$unused.Remaining;ResetAt=$unused.ResetAt;FetchedAt=$now}
Assert ((New-QuotaResetPacket '53197668' '12345678' 'claude' $unusedObservation $now.AddSeconds(4)) -ceq "LFR1,$marketing,12345678,claude,$marketing,0,4") 'Unused window publishes its actual observation age'
# Authenticated app-server response mocks: no sessions, database or production credentials.
$AccountFingerprint=$marketing
$script:limitServer=[Diagnostics.Process]::GetCurrentProcess()
$script:limitServerRetryAt=[DateTime]::MinValue
$script:limitServerRequestId=1
$script:identityReads=0; $script:switchIdentity=$false
function Read-QuotaAccountFingerprint($Process) {
    $script:identityReads++
    if ($script:switchIdentity -and $script:identityReads % 2 -eq 0) { return '00000000' }
    return $script:AccountFingerprint
}
function Send-AppServerLine($Process,$Message) { Assert ($Message.method -eq 'account/rateLimits/read') 'Actual quota query provenance' }
function Read-AppServerResponse($Process,$Id) { return $script:response }
$script:response='{"result":{"rateLimitsByLimitId":{"a":{"primary":{"usedPercent":25,"windowDurationMins":300,"resetsAt":1800001000},"secondary":{"usedPercent":60,"windowDurationMins":10080,"resetsAt":1800003600}},"b":{"primary":{"usedPercent":50,"windowDurationMins":300,"resetsAt":1800002000}}}}}' | ConvertFrom-Json
Assert ((Get-RemainingPercent) -eq 40) 'Existing minimum remaining semantics'
Assert ($codexResetQuota.ResetEpoch -eq 1800002000) 'Most exhausted explicit five-hour window; weekly reset never substituted'
Assert ((New-QuotaResetPacket $marketing '12345678' 'codex' $codexResetQuota $codexResetQuota.FetchedAt) -ceq "LFR1,$marketing,12345678,codex,$marketing,1800002000,0") 'Authenticated five-hour source to exact wire epoch'
Assert (([DateTimeOffset]::UtcNow-$codexResetQuota.FetchedAt).TotalSeconds -lt 2) 'Observation timestamp retained'
$script:response.result.rateLimitsByLimitId.b.primary.resetsAt=$null
Assert ((Get-RemainingPercent) -eq 40) 'Missing reset does not change quota'
Assert ((New-QuotaResetPacket $marketing '12345678' 'codex' $codexResetQuota $codexResetQuota.FetchedAt) -ceq "$prefix,0,0") 'Missing selected reset clears previous window'
$script:response='{"result":{"rateLimits":{"primary":{"usedPercent":60,"window_duration_mins":300,"reset_at":1800003000},"secondary":{"usedPercent":60,"resetsAt":1800004000}}}}' | ConvertFrom-Json
Assert ((Get-RemainingPercent) -eq 40) 'Legacy response unchanged'
Assert ($codexResetQuota.ResetEpoch -eq 1800003000) 'Legacy rateLimits and snake-case duration/reset fields'
$script:response='{"result":{"rateLimits":{"primary":{"usedPercent":60,"windowDurationMins":300,"resetsAt":1800001111},"secondary":{"usedPercent":60,"windowDurationMins":300,"resetsAt":1800002222}}}}' | ConvertFrom-Json
Assert ((Get-RemainingPercent) -eq 40) 'Equal five-hour limits preserve current balance'
Assert ($codexResetQuota.ResetEpoch -eq 1800001111) 'Equal five-hour candidates preserve source order'
# The paired account currently returns the same weekly-only snapshot in both fields.
$script:response='{"result":{"rateLimits":{"limitId":"codex","primary":{"usedPercent":23,"windowDurationMins":10080,"resetsAt":1795420842},"secondary":null},"rateLimitsByLimitId":{"codex":{"limitId":"codex","primary":{"usedPercent":23,"windowDurationMins":10080,"resetsAt":1795420842},"secondary":null}}}}' | ConvertFrom-Json
Assert ((Get-RemainingPercent) -eq 77) 'Weekly-only response preserves the reported balance'
$weeklyOnlyObservation=$codexResetQuota
Assert ((New-QuotaResetPacket $marketing '12345678' 'codex' $weeklyOnlyObservation $weeklyOnlyObservation.FetchedAt) -ceq "$prefix,1795420842,0") 'Weekly-only paired account sends the observed Codex reset'
foreach ($reset in @($null, $true, -1)) {
    $script:response.result.rateLimitsByLimitId.codex.primary.resetsAt=$reset
    Assert ((Get-RemainingPercent) -eq 77) 'Malformed weekly reset preserves balance'
    Assert ((New-QuotaResetPacket $marketing '12345678' 'codex' $codexResetQuota $codexResetQuota.FetchedAt) -ceq "$prefix,0,0") 'Malformed weekly reset clears earlier epoch'
}
# Explicit duration, not primary/secondary position or reset distance, defines 5h.
$script:response='{"result":{"rateLimits":{"primary":{"usedPercent":90,"windowDurationMins":10080,"resetsAt":1800009999},"secondary":{"usedPercent":20,"windowDurationMins":300,"resets_at":1800004444}}}}' | ConvertFrom-Json
Assert ((Get-RemainingPercent) -eq 10) 'Weekly-limited current stats preserved'
Assert ($codexResetQuota.ResetEpoch -eq 1800004444) 'Five-hour secondary position and resets_at alias'
foreach ($duration in @($null, 301, 10080, $true, '300.0')) {
    $script:response.result.rateLimits.secondary.windowDurationMins=$duration
    Assert ((Get-RemainingPercent) -eq 10) 'Missing/invalid duration does not alter balance'
    Assert ($codexResetQuota.ResetEpoch -eq 1800009999) 'Unsupported duration cannot displace the explicit weekly window'
}
$script:response.result.rateLimits.primary.windowDurationMins=$null
$script:response.result.rateLimits.secondary.windowDurationMins=301
Assert ((Get-RemainingPercent) -eq 10) 'Unknown durations preserve balance'
Assert ((New-QuotaResetPacket $marketing '12345678' 'codex' $codexResetQuota $codexResetQuota.FetchedAt) -ceq "$prefix,0,0") 'No reset inferred from unknown durations'
$script:response.result.rateLimits.primary.windowDurationMins=10080
$script:response.result.rateLimits.secondary.windowDurationMins=300
foreach ($reset in @($null, $true, '1800004444.0', '1.8e9', -1)) {
    $script:response.result.rateLimits.secondary.resets_at=$reset
    Assert ((Get-RemainingPercent) -eq 10) 'Invalid reset does not alter balance'
    Assert ((New-QuotaResetPacket $marketing '12345678' 'codex' $codexResetQuota $codexResetQuota.FetchedAt) -ceq "$prefix,0,0") 'Malformed timestamp clears previous reset'
}
# Claude five_hour source -> parsed ResetAt -> exact Unix seconds on the wire.
$claudeSource=@{five_hour=@{utilization=25.5;resets_at='2027-01-15T10:00:00+02:00'}}
$claudeNow=[DateTimeOffset]::Parse('2027-01-15T07:00:00Z')
$parsed=ConvertTo-ClaudeQuota $claudeSource $claudeNow
Assert ($parsed.Remaining -eq 74) 'Claude current rounding preserved'
$claudeObservation=@{Fingerprint=$marketing;ResetAt=$parsed.ResetAt;FetchedAt=$claudeNow}
$expectedEpoch=([DateTimeOffset]::Parse('2027-01-15T08:00:00Z')).ToUnixTimeSeconds()
Assert ((New-QuotaResetPacket $marketing '12345678' 'claude' $claudeObservation $claudeNow) -ceq "LFR1,$marketing,12345678,claude,$marketing,$expectedEpoch,0") 'Actual offset-aware Claude reset epoch transmitted'
$script:response='{"result":{"rateLimits":{"primary":{"usedPercent":60}}}}' | ConvertFrom-Json
$AccountFingerprint=Get-AccountFingerprint 'other@example.invalid'
Assert ((Get-RemainingPercent) -eq 40) 'Other paired identity keeps existing quota behavior'
Assert ($null -eq $codexResetQuota) 'Other authenticated identity cannot emit reset metadata'
$AccountFingerprint=$marketing
$script:switchIdentity=$true
Assert ((Get-RemainingPercent) -eq -1) 'Identity switch rejected'
Assert ($null -eq $codexResetQuota) 'Identity switch clears reset metadata'
$script:limitServer.Dispose(); $script:limitServer=$null
# Normal heartbeat: legacy packets unchanged, separate UDP/HTTP metadata.
. (Join-Path $PSScriptRoot 'test_quota_identity.ps1')
$script:DisableLimitPolling=$false
$weeklyOnlyObservation.FetchedAt=[DateTimeOffset]::UtcNow.AddSeconds(-42)
$script:codexResetQuota=$weeklyOnlyObservation
$script:claudePoller=@{Configured=$true;Quota=@{Fingerprint=$marketing;Remaining=74;ResetAt=[DateTimeOffset]::UtcNow.AddHours(1);FetchedAt=[DateTimeOffset]::UtcNow.AddSeconds(-12)}}
$script:resetHttpSenders=@{codex=@{};claude=@{}}
$script:nextResetReportAt=[DateTime]::MinValue
$script:nextClaudeReportAt=[DateTime]::MaxValue
$script:nextOriginHttpAt=[DateTime]::MaxValue
$script:packets.Clear()
$script:resetCalls=[Collections.Generic.List[object]]::new()
function Start-ClaudeHttpSend($State,$Bytes,$Address,$HttpPort) { $script:resetCalls.Add(@{State=$State;Packet=[Text.Encoding]::ASCII.GetString($Bytes)}) }
Send-Heartbeat
Assert ($packets.Count -eq 5 -and $resetCalls.Count -eq 2) 'Separate provider UDP and bounded HTTP sends'
Assert ($packets[3] -ceq $resetCalls[0].Packet -and $packets[4] -ceq $resetCalls[1].Packet) 'HTTP and UDP identical'
Assert ($packets[3] -ceq "$prefix,1795420842,42") 'Weekly-only app-server epoch and observation age sent on both transports'
Assert ($packets[4] -like 'LFR1,53197668,12345678,claude,*,*,12') 'Claude observation age sent'
Assert ([object]::ReferenceEquals($resetCalls[0].State,$resetHttpSenders.codex)) 'Dedicated Codex bounded sender'
Assert ([object]::ReferenceEquals($resetCalls[1].State,$resetHttpSenders.claude)) 'Dedicated Claude bounded sender'
Send-Heartbeat
Assert ($resetCalls.Count -eq 2 -and $packets.Count -eq 8) 'Five second throttle'
$script:DisableLimitPolling=$true; $script:nextResetReportAt=[DateTime]::MinValue
Send-Heartbeat
Assert ($resetCalls.Count -eq 2 -and $packets.Count -eq 11) 'Disabled polling suppresses reset metadata'
Write-Host 'PASS LFR1 schema, strict bounds, proven identity, observation age, five-hour preference, weekly fallback, exact source epochs, legacy semantics, UDP/HTTP routing and throttle.'
