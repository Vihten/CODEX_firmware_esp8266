$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'codex_lan_agent.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in @('Get-AccountFingerprint','ConvertTo-ClaudeQuota','Test-ClaudeQuotaFresh','New-ClaudeQuotaFailure','Get-ClaudeHttpFailure','Test-ClaudeCredentialPath','Read-ClaudeCredentialText','Read-ClaudeQuotaIdentity','Read-ClaudeUsableDesktopCredential','Copy-ClaudeVerifiedProfile','Read-ClaudeQuota','New-ClaudeQuotaPoller','Set-ClaudePollFailure','Stop-ClaudeQuotaWorker','Update-ClaudeQuotaPoller','New-ClaudeQuotaPacket','Update-ClaudeHttpSender','Start-ClaudeHttpSend')) {
    $node = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true)
    . ([scriptblock]::Create($node.Extent.Text))
}
function Assert($Value, $Message) { if (-not $Value) { throw $Message } }
$script:quotaMessages = [Collections.Generic.List[string]]::new()
function Write-AgentStatus([string]$Message) { $script:quotaMessages.Add($Message) }
function Initialize-ClaudeDesktopCrypto { }
function Read-ClaudeDesktopCredential($Identity) { return $script:desktopGrant }
$now = [DateTimeOffset]::UtcNow
$usage = @{ five_hour = @{ utilization = 25.5; resets_at = $now.AddHours(1).ToString('o') } }
$usage.five_hour.resets_at = $now.AddHours(1).UtcDateTime
Assert ($null -ne (ConvertTo-ClaudeQuota $usage $now)) 'Native REST DateTime reset accepted'
$usage.five_hour.resets_at = $now.AddHours(1).ToString('o')
$q = ConvertTo-ClaudeQuota $usage $now
Assert ($q.Remaining -eq 74) 'Conservative rounding'
foreach ($bad in @(-1,101,[double]::NaN,[double]::PositiveInfinity,'25',$true,$null)) {
    $usage.five_hour.utilization = $bad
    Assert ($null -eq (ConvertTo-ClaudeQuota $usage $now)) 'Reject malformed utilization'
}
$usage.five_hour.utilization = 0
foreach ($reset in @('bad','2026-01-01', $now.AddSeconds(-1).ToString('o'),$now.AddHours(6).ToString('o'))) {
    $usage.five_hour.resets_at = $reset
    Assert ($null -eq (ConvertTo-ClaudeQuota $usage $now)) 'Reject invalid reset'
}
# An unused authenticated window has no reset until consumption begins.
$unused = ConvertTo-ClaudeQuota @{five_hour=@{utilization=0;resets_at=$null}} $now
Assert ($null -ne $unused -and $unused.Remaining -eq 100 -and $null -eq $unused.ResetAt) 'Explicit numeric zero with null reset is unused quota'
foreach ($used in @(0.1,1,100)) {
    Assert ($null -eq (ConvertTo-ClaudeQuota @{five_hour=@{utilization=$used;resets_at=$null}} $now)) 'Nonzero usage requires reset'
}
foreach ($window in @(@{resets_at=$null},@{utilization=$null;resets_at=$null},@{utilization='0';resets_at=$null},@{utilization=$false;resets_at=$null},@{utilization='bad';resets_at=$null})) {
    Assert ($null -eq (ConvertTo-ClaudeQuota @{five_hour=$window} $now)) 'Missing or malformed usage must not become 100 percent'
}
$unusedState = @{Quota=@{Remaining=100;Fingerprint=(Get-AccountFingerprint 'marketing@tiras.ua');FetchedAt=$now;ResetAt=$null}}
foreach ($age in @(0,30,89.999)) {
    $instant=$now.AddSeconds($age)
    Assert (Test-ClaudeQuotaFresh $unusedState.Quota $instant) 'Unused observation fresh within 90 seconds'
    $ttl=[int][Math]::Floor(90000-($instant-$now).TotalMilliseconds)
    Assert ((New-ClaudeQuotaPacket $unusedState '53197668' '12345678' $instant) -ceq "LFC1,53197668,12345678,100,$(Get-AccountFingerprint 'marketing@tiras.ua'),$ttl") 'Unused packet TTL decreases from observation'
}
foreach ($age in @(-0.001,90,120)) {
    $instant=$now.AddSeconds($age)
    Assert (-not (Test-ClaudeQuotaFresh $unusedState.Quota $instant)) 'Future or expired unused observation rejected'
    Assert ((New-ClaudeQuotaPacket $unusedState '53197668' '12345678' $instant) -ceq 'LFC1,53197668,12345678,-1,00000000,0') 'Future or expired unused packet unknown'
}
$unusedState.Quota.Remaining=99
Assert (-not (Test-ClaudeQuotaFresh $unusedState.Quota $now)) 'Nonzero usage with null reset is not fresh'
$unusedState.Quota.Remaining=100
$state = @{ Quota = @{ Remaining=74; Fingerprint=(Get-AccountFingerprint 'marketing@tiras.ua'); FetchedAt=$now; ResetAt=$now.AddHours(1) } }
Assert ((New-ClaudeQuotaPacket $state '53197668' '12345678' $now) -eq "LFC1,53197668,12345678,74,$(Get-AccountFingerprint 'marketing@tiras.ua'),90000") 'Verified packet'
Assert ((New-ClaudeQuotaPacket $state '53197668' '12345678' $now.AddSeconds(90)) -eq 'LFC1,53197668,12345678,-1,00000000,0') '90 second expiry'
Assert ((New-ClaudeQuotaPacket $state '53197668' '12345678' $now.AddSeconds(-1)) -eq 'LFC1,53197668,12345678,-1,00000000,0') 'Future fetched date'
Assert ((New-ClaudeQuotaPacket $state '53197668' '12345678' $now.AddSeconds(30)) -like '*,60000') 'Repeated packet decreases validity'
$state.Quota.ResetAt = $now.AddSeconds(5)
Assert ((New-ClaudeQuotaPacket $state '53197668' '12345678' $now) -like '*,5000') 'Reset caps validity'
$state.Quota.ResetAt = $now
Assert ((New-ClaudeQuotaPacket $state '53197668' '12345678' $now) -like '*,-1,00000000,0') 'Reset expiry'
# Synthetic grants only. Production credential and Desktop readers are never invoked here.
$script:identityText = '{"oauthAccount":{"emailAddress":"marketing@tiras.ua","accountUuid":"account","organizationUuid":"org"},"projects":{"C:/Project":{},"c:/project":{}}}'
$parsedIdentity = Read-ClaudeQuotaIdentity $script:identityText
Assert ($parsedIdentity.emailAddress -ceq 'marketing@tiras.ua') 'Unrelated keys differing only by case accepted'
foreach ($bad in @('{}','{"OAuthAccount":{}}','{"oauthAccount":{"emailAddress":["marketing@tiras.ua"],"accountUuid":"account","organizationUuid":"org"}}','{bad',(' ' * (4MB + 1)))) {
    $rejected = $false
    try { [void](Read-ClaudeQuotaIdentity $bad) } catch { $rejected = $true }
    Assert $rejected 'Malformed or incorrectly typed identity rejected'
}
function Read-ClaudeCredentialText($Path) {
    if ($Path -like '*/.claude.json' -or $Path -like '*\.claude.json') { return $script:identityText }
    $expiry = 9999999999999; if ($script:grantExpiry) { $expiry=$script:grantExpiry }; if ($script:expired) { $expiry = 1 }
    return (@{claudeAiOauth=@{accessToken='cli-mock';expiresAt=$expiry;organizationUuid='org';refreshToken='never-use-mock'}} | ConvertTo-Json -Compress)
}
function Invoke-RestMethod { param($Uri,$Headers,$TimeoutSec,$MaximumRedirection,$Method,$Body)
    $script:calls++
    Assert (-not $Method -or $Method -eq 'Get') 'Quota reader never posts or refreshes tokens'
    Assert ($Uri -in @('https://api.anthropic.com/api/oauth/profile','https://api.anthropic.com/api/oauth/usage')) 'Only read-only official quota endpoints'
    Assert ($TimeoutSec -eq 4 -and $MaximumRedirection -eq 0) 'Requests bounded without redirects'
    if ($script:apiError) { throw $script:apiError }
    if ($Uri -like '*/profile') {
        if ($script:profile401 -and $Headers.Authorization -eq 'Bearer cli-mock') {
            $exception = [Exception]::new('private-mock-response')
            $exception | Add-Member -NotePropertyName Response -NotePropertyValue ([pscustomobject]@{StatusCode=401;Headers=@{}})
            throw $exception
        }
        $email = 'marketing@tiras.ua'; if ($script:wrongEmail) { $email = 'other@example.invalid' }
        return @{account=@{email=$email;uuid='account'};organization=@{uuid='org'}}
    }
    if ($script:usageError) { throw $script:usageError }
    if ($null -ne $script:usageFixture) { return $script:usageFixture }
    return @{five_hour=@{utilization=22;resets_at=[DateTimeOffset]::UtcNow.AddHours(1).ToString('o')}}
}
$script:calls=0
$result = Read-ClaudeQuota
Assert ($result.Kind -eq 'QuotaSuccess' -and $result.Source -eq 'cli' -and $result.Remaining -eq 78 -and $script:calls -eq 2) 'Normal CLI quota'
$script:usageFixture=@{five_hour=@{utilization=0;resets_at=$null}}
$readUnused=Read-ClaudeQuota
Assert ($readUnused.Kind -eq 'QuotaSuccess' -and $readUnused.Remaining -eq 100 -and $null -eq $readUnused.ResetAt) 'Read-ClaudeQuota accepts authenticated unused window'
Assert (Test-ClaudeQuotaFresh $readUnused $readUnused.FetchedAt) 'Reader unused observation is fresh'
Assert ((New-ClaudeQuotaPacket @{Quota=$readUnused} '53197668' '12345678' $readUnused.FetchedAt) -like 'LFC1,53197668,12345678,100,*,90000') 'Reader unused result reaches LFC1'
foreach ($window in @(@{utilization=1;resets_at=$null},@{resets_at=$null},@{utilization='0';resets_at=$null},@{utilization=$null;resets_at=$null})) {
    $script:usageFixture=@{five_hour=$window}
    Assert ((Read-ClaudeQuota).Kind -ne 'QuotaSuccess') 'Reader rejects nonzero null-reset and malformed usage'
}
$script:usageFixture=$null
$verified = $result.VerifiedProfile
Assert ($null -ne $verified -and $verified.GrantDigest.Length -eq 64) 'Successful profile produces digest metadata'
Assert (($verified | ConvertTo-Json -Compress) -notmatch 'cli-mock|accessToken|refreshToken') 'Cache contains no credentials'
$script:calls=0
$cached = Read-ClaudeQuota $verified
Assert ($cached.Kind -eq 'QuotaSuccess' -and $script:calls -eq 1) 'Verified profile skips GET'
Assert ($cached.VerifiedProfile.CheckedAt -eq $verified.CheckedAt -and $cached.VerifiedProfile.ExpiresAt -eq $verified.ExpiresAt) 'Repeated quota preserves verification timestamps'
foreach ($field in @('GrantDigest','AccountUuid','OrganizationUuid','EmailAddress','CheckedAt','ExpiresAt')) {
    $changed = $verified | Select-Object *
    switch ($field) {
        'GrantDigest' { $changed.$field = 'A' * 64 }
        'CheckedAt' { $changed.$field = [DateTimeOffset]::UtcNow.AddSeconds(30) }
        'ExpiresAt' { $changed.$field = [DateTimeOffset]::UtcNow.AddSeconds(-1) }
        default { $changed.$field = 'changed' }
    }
    $script:calls=0
    Assert ((Read-ClaudeQuota $changed).Kind -eq 'QuotaSuccess' -and $script:calls -eq 2) "Changed $field requires profile"
}
$changed = $verified | Select-Object *
$changed.CheckedAt = [DateTimeOffset]::UtcNow.AddMinutes(-15)
$script:calls=0
Assert ((Read-ClaudeQuota $changed).Kind -eq 'QuotaSuccess' -and $script:calls -eq 2) 'Fifteen minute cache expires'
Assert ($null -eq (Copy-ClaudeVerifiedProfile @{ GrantDigest='A'*64; AccountUuid='account'; OrganizationUuid='org'; EmailAddress='marketing@tiras.ua'; CheckedAt='bad'; ExpiresAt='bad' })) 'Malformed cache timestamps rejected'
foreach ($status in @(429,401,403)) {
    $exception=[Exception]::new('synthetic usage failure')
    $exception | Add-Member -NotePropertyName Response -NotePropertyValue ([pscustomobject]@{StatusCode=$status;Headers=@{}})
    $script:usageError=$exception; $script:calls=0
    $failed = Read-ClaudeQuota $verified
    Assert ($failed.HttpStatus -eq $status -and $script:calls -eq 1) 'Usage failures handled directly'
    if ($status -eq 429) {
        Assert ($failed.VerifiedProfile.CheckedAt -eq $verified.CheckedAt) 'Usage 429 retains verified profile'
        $script:calls=0
        $freshFailure=Read-ClaudeQuota
        Assert ($freshFailure.VerifiedProfile.CheckedAt -ge $verified.CheckedAt -and $script:calls -eq 2) 'Freshly verified profile retained on usage 429'
    } else { Assert ($null -eq $failed.VerifiedProfile) 'Auth failure invalidates profile' }
}
$script:usageError=$null
$originalIdentity=$script:identityText
$script:identityText=$originalIdentity.Replace('"account"','"switched-account"')
$script:calls=0
Assert ((Read-ClaudeQuota $verified).Failure -eq 'identity' -and $script:calls -eq 1) 'Current account switch forces profile validation'
$script:identityText=$originalIdentity
$script:grantExpiry=[DateTimeOffset]::UtcNow.AddMinutes(2).ToUnixTimeMilliseconds()
$script:calls=0
$shortGrant=Read-ClaudeQuota $verified
Assert ($script:calls -eq 2 -and $shortGrant.VerifiedProfile.ExpiresAt.ToUnixTimeMilliseconds() -eq $script:grantExpiry) 'Shortened grant expiry forces profile and bounds cache'
$script:grantExpiry=$null
$script:expired=$true; $script:calls=0
Assert ((Read-ClaudeQuota).Failure -eq 'expired' -and $script:calls -eq 0) 'Expired CLI waits for owner login without refresh'
$script:desktopGrant=[pscustomobject]@{accessToken='desktop-mock';expiresAt=9999999999999;Source='desktop'}
$script:calls=0
$result=Read-ClaudeQuota $verified
Assert ($script:calls -eq 2) 'Different Desktop grant cannot reuse CLI verification'
Assert ($result.Kind -eq 'QuotaSuccess' -and $result.Source -eq 'desktop' -and $result.Remaining -eq 78) 'Expired CLI prefers valid Desktop grant'
$script:wrongEmail=$true; $script:calls=0
Assert ((Read-ClaudeQuota).Failure -eq 'identity' -and $script:calls -eq 1) 'Desktop profile must match marketing identity before usage'
$script:wrongEmail=$false; $script:expired=$false; $script:profile401=$true; $script:calls=0
$result=Read-ClaudeQuota
Assert ($result.Kind -eq 'QuotaSuccess' -and $result.Source -eq 'desktop' -and $script:calls -eq 3) 'CLI profile 401 uses Desktop exactly once'
$script:desktopGrant=$null; $script:calls=0
Assert ((Read-ClaudeQuota).Failure -eq 'auth' -and $script:calls -eq 1) 'CLI 401 with no Desktop grant never refreshes'
$script:profile401=$false
$exception=[Exception]::new('private-mock-response')
$exception | Add-Member -NotePropertyName Response -NotePropertyValue ([pscustomobject]@{StatusCode=429;Headers=@{'Retry-After'='600'}})
$script:apiError=$exception
$result=Read-ClaudeQuota
Assert ($result.HttpStatus -eq 429 -and $result.RetrySeconds -eq 600 -and $result.Failure -eq 'api') 'Actual HTTP 429 metadata parsing'
Assert (($result | ConvertTo-Json -Compress) -notmatch 'private|mock|Bearer|token') 'Failure output sanitized'
$exception.Response.Headers['Retry-After']='1'
Assert ((Read-ClaudeQuota).RetrySeconds -eq 120) 'Retry-After minimum 120 seconds'
$exception.Response.Headers['Retry-After']='999999999999'
Assert ((Read-ClaudeQuota).RetrySeconds -eq 86400) 'Retry-After capped at one day'
$script:apiError=$null
# Exercise the bounded reader with a synthetic file, including growth/oversize rejection.
$node=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Read-ClaudeCredentialText'},$true)
. ([scriptblock]::Create($node.Extent.Text))
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('claude-read-test-'+[guid]::NewGuid().ToString('N'))
try {
    [IO.File]::WriteAllText($fixture, '{"synthetic":true}')
    Assert ((Read-ClaudeCredentialText $fixture | ConvertFrom-Json).synthetic) 'Bounded credential read'
    $stream=[IO.File]::OpenWrite($fixture); try { $stream.SetLength(4MB+1) } finally { $stream.Dispose() }
    $rejected=$false; try { [void](Read-ClaudeCredentialText $fixture) } catch { $rejected=$true }
    Assert $rejected 'Oversized credential file rejected before allocation'
    Assert (-not (Test-ClaudeCredentialPath '\\server\share\credentials.json')) 'UNC rejected'
    Assert (-not (Test-ClaudeCredentialPath ($fixture+':stream'))) 'Alternate stream rejected'
}
finally { [IO.File]::Delete($fixture) }
Assert ($ast.Extent.Text -notmatch 'function (Save-ClaudeCredentialRefresh|Request-ClaudeQuotaRefresh)|platform.claude.com/v1/oauth/token') 'Runtime has no CLI grant mutation path'
Write-Host 'PASS read-only CLI/Desktop selection, marketing profile, no refresh/write, bounded reads, 429 clamps and sanitized errors.'
$backoff = New-ClaudeQuotaPoller
$script:quotaMessages.Clear()
foreach ($seconds in @(15,30,60,120,240,480,900,900)) {
    Set-ClaudePollFailure $backoff (New-ClaudeQuotaFailure 'network') $now
    Assert (($backoff.NextPoll - $now).TotalSeconds -eq $seconds) 'Bounded exponential transient retries'
}
Assert ($script:quotaMessages.Count -eq 1) 'Repeated identical failures logged once'
Set-ClaudePollFailure $backoff (New-ClaudeQuotaFailure 'auth' 401) $now
Assert ($script:quotaMessages.Count -eq 2 -and $backoff.HttpStatus -eq 401 -and $backoff.LastFailure -eq 'auth') 'Changed failure metadata logged'
$backoff = New-ClaudeQuotaPoller
Set-ClaudePollFailure $backoff (New-ClaudeQuotaFailure 'api' 429 600) $now
Assert (($backoff.NextPoll - $now).TotalSeconds -eq 600) 'Server delay overrides transient retry'
Assert ($backoff.PollSeconds -eq 120) 'First 429 doubles normal 60 second interval'
Set-ClaudePollFailure $backoff (New-ClaudeQuotaFailure 'api' 429) $now
Assert ($backoff.PollSeconds -eq 180 -and ($backoff.NextPoll-$now).TotalSeconds -eq 180) 'Repeated 429 keeps 180 second cap'
foreach ($status in @(401,403)) {
    $backoff.VerifiedProfile=$verified
    Set-ClaudePollFailure $backoff (New-ClaudeQuotaFailure 'auth' $status) $now
    Assert ($null -eq $backoff.VerifiedProfile) 'Poller auth clears cache'
}
Assert (($script:quotaMessages -join ' ') -notmatch 'secret|mock|Bearer|refreshToken') 'Logs contain curated status only'
Write-Host 'PASS 15/30/60/120/240/480/900 retry schedule, state-change logging and Retry-After scheduling.'
# Synthetic reader runs in the production poller runspace; no credentials or requests.
function Read-ClaudeQuota { [pscustomobject]@{Kind='QuotaSuccess';Source='cli';Remaining=100;ResetAt=$null;FetchedAt=[DateTimeOffset]::UtcNow;Fingerprint=(Get-AccountFingerprint 'marketing@tiras.ua')} }
$poller=New-ClaudeQuotaPoller
Assert ($poller.PollSeconds -eq 60) 'Normal polling refreshes within 90 second TTL'
try {
    Update-ClaudeQuotaPoller $poller
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    while (-not $poller.Pending.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 10 }
    $commitNow=[DateTimeOffset]::UtcNow
    Update-ClaudeQuotaPoller $poller $commitNow
    Assert ($null -ne $poller.Quota -and $poller.Quota.Remaining -eq 100 -and $null -eq $poller.Quota.ResetAt) 'Poller commits unused null-reset quota'
    Assert (($poller.NextPoll-$commitNow).TotalSeconds -eq 60) 'Successful poll schedules 60 seconds'
    $observed=$poller.Quota.FetchedAt
    Assert ((New-ClaudeQuotaPacket $poller '53197668' '12345678' $observed) -like 'LFC1,53197668,12345678,100,*,90000') 'Poller unused result reaches bounded LFC1'
    $poller.NextPoll=$observed.AddHours(1)
    Update-ClaudeQuotaPoller $poller $observed.AddSeconds(89)
    Assert ($null -ne $poller.Quota) 'Poller retains unused observation before TTL'
    Update-ClaudeQuotaPoller $poller $observed.AddSeconds(90)
    Assert ($null -eq $poller.Quota) 'Poller clears unused observation at TTL'
    $poller.Quota=$unusedState.Quota
    Update-ClaudeQuotaPoller $poller $now.AddMilliseconds(-1)
    Assert ($null -eq $poller.Quota) 'Poller clears future unused observation'
} finally { Stop-ClaudeQuotaWorker $poller }
foreach ($offset in @(-90,1)) {
    function Read-ClaudeQuota { [pscustomobject]@{Kind='QuotaSuccess';Remaining=100;ResetAt=$null;FetchedAt=[DateTimeOffset]::UtcNow.AddSeconds(0);Fingerprint=(Get-AccountFingerprint 'marketing@tiras.ua')} }
    Set-Item function:Read-ClaudeQuota ([scriptblock]::Create((Get-Item function:Read-ClaudeQuota).Definition.Replace('AddSeconds(0)', ('AddSeconds(' + [string]$offset + ')'))))
    $poller=New-ClaudeQuotaPoller
    try {
        Update-ClaudeQuotaPoller $poller
        $deadline=[DateTime]::UtcNow.AddSeconds(5)
        while (-not $poller.Pending.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 10 }
        Update-ClaudeQuotaPoller $poller
        Assert ($null -eq $poller.Quota -and $poller.Failures -eq 1) 'Poller rejects expired or future worker observation'
    } finally { Stop-ClaudeQuotaWorker $poller }
}
# Real separate runspace runs only synthetic functions, with no local file access.
function Read-ClaudeQuota($VerifiedProfile) { [pscustomobject]@{ VerifiedProfile=$VerifiedProfile; Kind='QuotaSuccess'; Remaining=66; ResetAt=[DateTimeOffset]::UtcNow.AddHours(1); FetchedAt=[DateTimeOffset]::UtcNow; Fingerprint=(Get-AccountFingerprint 'marketing@tiras.ua') } }
$poller = New-ClaudeQuotaPoller
$poller.VerifiedProfile=$verified; $poller.PollSeconds=180
$poller.LastFailure = 'network'; $poller.LastStatus = 'network/0'; $poller.Failures = 2
try {
    Update-ClaudeQuotaPoller $poller
    Assert ($null -ne $poller.Pending) 'Asynchronous worker started'
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $poller.Pending.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 10 }
    Update-ClaudeQuotaPoller $poller
    Assert ($poller.Quota.Remaining -eq 66) 'Runspace result committed'
    Assert ($poller.VerifiedProfile.GrantDigest -ceq $verified.GrantDigest) 'Worker receives verified cache through AddArgument'
    Assert ($poller.VerifiedProfile.CheckedAt -eq $verified.CheckedAt) 'Worker preserves verification timestamp'
    Assert (($poller.NextPoll - [DateTimeOffset]::UtcNow).TotalSeconds -gt 175 -and $poller.PollSeconds -eq 180) 'Success retains learned interval'
    Assert ($null -eq $poller.Worker) 'Worker disposed'
    Assert ($poller.LastFailure -eq '' -and $poller.HttpStatus -eq 0 -and $poller.Failures -eq 0) 'Recovery clears failure state'
    Assert ($script:quotaMessages[$script:quotaMessages.Count - 1] -eq 'Claude quota recovered (source=cli, remaining=66%).') 'Recovery logged once with curated source and quota'
    $messageCount = $script:quotaMessages.Count
    $poller.NextPoll = [DateTimeOffset]::MinValue
    Update-ClaudeQuotaPoller $poller
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $poller.Pending.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 10 }
    Update-ClaudeQuotaPoller $poller
    Assert ($script:quotaMessages.Count -eq $messageCount) 'Unchanged successful poll produces no log spam'
    function Read-ClaudeQuota { [pscustomobject]@{ Kind='QuotaSuccess'; Source='desktop'; Remaining=78; ResetAt=[DateTimeOffset]::UtcNow.AddHours(1); FetchedAt=[DateTimeOffset]::UtcNow; Fingerprint=(Get-AccountFingerprint 'marketing@tiras.ua') } }
    $poller.NextPoll = [DateTimeOffset]::MinValue
    Update-ClaudeQuotaPoller $poller
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $poller.Pending.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 10 }
    Update-ClaudeQuotaPoller $poller
    Assert ($script:quotaMessages.Count -eq ($messageCount + 1) -and $poller.LastSource -eq 'desktop') 'Source change logged once'
    Assert ($script:quotaMessages[$script:quotaMessages.Count - 1] -eq 'Claude quota ready (source=desktop, remaining=78%).') 'Source acquisition identifies sanitized quota'
} finally { Stop-ClaudeQuotaWorker $poller }
function Read-ClaudeQuota($VerifiedProfile) { $failure=New-ClaudeQuotaFailure 'api' 429 600; $failure | Add-Member -NotePropertyName VerifiedProfile -NotePropertyValue $VerifiedProfile; $failure }
$poller = New-ClaudeQuotaPoller
$poller.VerifiedProfile=$verified
try {
    Update-ClaudeQuotaPoller $poller
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $poller.Pending.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 10 }
    Update-ClaudeQuotaPoller $poller
    Assert ($null -eq $poller.Quota) 'Rate limiting cannot publish a quota'
    Assert ($poller.VerifiedProfile.CheckedAt -eq $verified.CheckedAt) 'Poller adopts cache even on worker failure'
    Assert (($poller.NextPoll - [DateTimeOffset]::UtcNow).TotalSeconds -gt 595) 'Retry-After respected'
} finally { Stop-ClaudeQuotaWorker $poller }
function Read-ClaudeQuota($VerifiedProfile) {
    $failure=New-ClaudeQuotaFailure 'api' 429
    $VerifiedProfile.CheckedAt='invalid-date'
    $failure | Add-Member -NotePropertyName VerifiedProfile -NotePropertyValue $VerifiedProfile
    $failure
}
$poller = New-ClaudeQuotaPoller
$poller.VerifiedProfile=Copy-ClaudeVerifiedProfile $verified
try {
    Update-ClaudeQuotaPoller $poller
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $poller.Pending.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 10 }
    Update-ClaudeQuotaPoller $poller
    Assert ($null -eq $poller.VerifiedProfile) 'Malformed worker cache safely discarded'
} finally { Stop-ClaudeQuotaWorker $poller }
function Read-ClaudeQuota { Start-Sleep -Seconds 30 }
$poller = New-ClaudeQuotaPoller
try {
    Update-ClaudeQuotaPoller $poller
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Update-ClaudeQuotaPoller $poller ([DateTimeOffset]::UtcNow.AddSeconds(21))
    Assert ($watch.Elapsed.TotalSeconds -lt 1) 'Cancellation must not block heartbeat'
    Assert ($null -ne $poller.Stopping) 'Timeout initiates cancellation'
} finally { Stop-ClaudeQuotaWorker $poller }
# Slow synthetic HTTP occupies only one worker and never blocks the caller.
function Send-DirectHttpPacket($Bytes) { Start-Sleep -Seconds 30 }
$sender = @{ Worker=$null; Pending=$null; Stopping=$null; Started=[DateTimeOffset]::MinValue }
try {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Start-ClaudeHttpSend $sender ([byte[]]@(65)) '127.0.0.1' 80
    $firstWorker = $sender.Worker
    Start-ClaudeHttpSend $sender ([byte[]]@(66)) '127.0.0.1' 80
    Assert ($watch.Elapsed.TotalMilliseconds -lt 750) 'HTTP start never blocks heartbeat'
    Assert ([object]::ReferenceEquals($firstWorker,$sender.Worker)) 'HTTP tasks cannot accumulate'
    $sender.Started = [DateTimeOffset]::UtcNow.AddSeconds(-1)
    Update-ClaudeHttpSender $sender
    Assert ($null -ne $sender.Stopping) 'HTTP cancellation after 750ms'
} finally { Stop-ClaudeQuotaWorker $sender }
# Use the established heartbeat harness to preserve LFA3 assertions and inspect routing.
. (Join-Path $PSScriptRoot 'test_quota_identity.ps1')
$node = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Send-Heartbeat' }, $true)
. ([scriptblock]::Create($node.Extent.Text))
$script:claudePoller = @{ Configured=$true; Quota=$null }
$script:nextClaudeReportAt = [DateTime]::MinValue
$script:DisableLimitPolling = $false
$script:httpPackets = [Collections.Generic.List[string]]::new()
function Start-ClaudeHttpSend($State,$Bytes,$Address,$HttpPort) { $script:httpPackets.Add([Text.Encoding]::ASCII.GetString($Bytes)) }
$packets.Clear()
Send-Heartbeat
Assert ($packets[3] -eq 'LFC1,53197668,12345678,-1,00000000,0') 'Configured unknown Claude UDP packet'
Assert ($httpPackets[0] -eq $packets[3]) 'Claude HTTP fallback uses same packet'
$packets.Clear(); $httpPackets.Clear()
$script:nextClaudeReportAt = [DateTime]::MinValue
$script:DisableLimitPolling = $true
Send-Heartbeat
Assert ($packets.Count -eq 3 -and $httpPackets.Count -eq 0) 'DisableLimitPolling suppresses Claude'
Write-Host 'PASS verified profile cache, grant/identity switches, expiry, auth invalidation, worker arguments and adaptive 429.'
Write-Host 'PASS Claude validation, identity mocks, freshness, packet schema, separate runspace and cleanup.'
