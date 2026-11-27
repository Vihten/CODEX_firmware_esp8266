[CmdletBinding()]
param(
    [string]$CollectorHost = '255.255.255.255',
    [ValidateRange(1, 65535)]
    [int]$Port = 4212,
    [ValidateRange(250, 5000)]
    [int]$PollMilliseconds = 1000,
    [string]$SessionsRoot = (Join-Path $env:USERPROFILE '.codex\sessions'),
    [string]$LogPath = '',
    [string]$DirectHost = '255.255.255.255',
    [ValidateRange(1, 65535)]
    [int]$DirectPort = 4210,
    [ValidateRange(1, 65535)]
    [int]$DirectHttpPort = 80,
    [ValidatePattern('^[0-9A-Fa-f]{8}$')]
    [string]$AccountFingerprint = '53197668',
    [ValidateRange(15, 3600)]
    [int]$LimitPollSeconds = 15,
    [switch]$DisableLimitPolling,
    [string]$InstanceName = 'LEDForgeCodexLanAgent',
    [string]$AgentName = '',
    [string]$ColorHex6 = '',
    [string]$MetadataPath = '',
    [string]$ControllerStatePath = '',
    [string]$TokenStatePath = '',
    [string]$UpdateControlPath = '',
    [switch]$EnableChat,
    [string]$ClaudeRoot = $(if ($env:CLAUDE_CONFIG_DIR) { Join-Path $env:CLAUDE_CONFIG_DIR 'projects' }
        else { Join-Path $env:USERPROFILE '.claude\projects' })
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'agent_metadata.ps1')
. (Join-Path $PSScriptRoot 'weekly_tokens.ps1')
if ([string]::IsNullOrWhiteSpace($TokenStatePath)) {
    $TokenStatePath = Join-Path $PSScriptRoot 'weekly-token-state.json'
}
$weeklyTokens = $null
$nextTokenScan = [DateTime]::MinValue
$weeklyTokensReady = $false
$codexDailyTokens = $null
$nextDailyReportAt = [DateTime]::MinValue
$tokenScanner = $null
$claudeDailyScanner = $null
$claudeDailyTokens = $null
$nextClaudeDailyScan = [DateTime]::MinValue
$preferClaudeDaily = $false
$lastTokenSendWasWeekly = $false
$claudePoller = $null
$claudeHttpSender = $null
$originHttpSender = $null
$resetHttpSenders = @{}
$nextResetReportAt = [DateTime]::MinValue
$codexResetQuota = $null
$nextOriginHttpAt = [DateTime]::MinValue
$nextClaudeReportAt = [DateTime]::MinValue
if ([string]::IsNullOrWhiteSpace($MetadataPath)) {
    $MetadataPath = Join-Path $PSScriptRoot 'agent-metadata.json'
}
if (Test-Path -LiteralPath $MetadataPath) {
    $savedMetadata = Get-Content -LiteralPath $MetadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $PSBoundParameters.ContainsKey('AgentName')) { $AgentName = $savedMetadata.Name }
    if (-not $PSBoundParameters.ContainsKey('ColorHex6')) { $ColorHex6 = $savedMetadata.ColorHex6 }
}
$metadata = Get-AgentMetadata -Name $AgentName -ColorHex6 $ColorHex6
$script:effectiveColorHex6 = $metadata.ColorHex6

$clientId = "$env:COMPUTERNAME-$env:USERNAME"
$sha256 = [Security.Cryptography.SHA256]::Create()
$clientHashBytes = $sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($clientId))
$clientHash = -join ($clientHashBytes[0..3] | ForEach-Object { $_.ToString('X2') })
$sha256.Dispose()
$udp = [System.Net.Sockets.UdpClient]::new()
$udp.EnableBroadcast = $true
$discoveryUdp = $null
try { $discoveryUdp = [Net.Sockets.UdpClient]::new($DirectPort) }
catch [Net.Sockets.SocketException] { # Another preview may already listen on this port.
}
$offsets = @{}
$remainders = @{}
$sessionActive = @{}
$sessionLastWrite = @{}
$nextSessionDiscovery = [DateTime]::MinValue
$claudeActivityState = @{}
$claudeDiscoveryStack = [Collections.Generic.List[object]]::new()
$nextClaudeActivityDiscovery = [DateTime]::MinValue
$boostUntil = [DateTime]::MinValue
$boostActivity = 0.0
$activity = 0.0
$script:codexActivity = 0.0
$script:claudeActivity = 0.0
$lastReportedActivity = -1.0
$remainingPercent = -1
$quotaFingerprint = '00000000'
$nextLimitPoll = [DateTime]::MinValue
$script:limitServer = $null
$script:limitServerRequestId = 1
$script:limitServerRetryAt = [DateTime]::MinValue
$directStartedAt = Get-Date
$lastDirectAckAt = [DateTime]::MinValue
$directStatusKnown = $false
$directConnected = $false
$nextDirectHttpAt = [DateTime]::MinValue
$nextWeeklyReportAt = [DateTime]::MinValue
$configuredDirectHost = $DirectHost
$controllerDeviceId = ''
$controllerCacheDirty = $false
$discoveryCandidates = @()
$discoveryIndex = 0
$nextDiscoveryAt = [DateTime]::MinValue
if ([string]::IsNullOrWhiteSpace($ControllerStatePath)) {
    $ControllerStatePath = Join-Path $PSScriptRoot 'controller-endpoint.json'
}
try {
    if (Test-Path -LiteralPath $ControllerStatePath) {
        $savedController = Get-Content -LiteralPath $ControllerStatePath -Raw | ConvertFrom-Json
        $savedIp = $null
        if ($savedController.Seed -eq $configuredDirectHost -and
            $savedController.Account -eq $AccountFingerprint -and
            $savedController.Port -eq $DirectPort -and
            $savedController.HttpPort -eq $DirectHttpPort -and
            $savedController.Device -cmatch '\A[0-9A-Fa-f]{6}\z' -and
            [Net.IPAddress]::TryParse([string]$savedController.Host, [ref]$savedIp) -and
            $savedIp.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork) {
            $DirectHost = $savedController.Host
            $controllerDeviceId = $savedController.Device.ToUpperInvariant()
        }
    }
}
catch { # A missing or damaged optional cache must not stop telemetry.
}
$confirmedDirectHost = $DirectHost

function Write-AgentStatus([string]$Message) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $Message"
    Write-Host $line
    if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
        try {
            Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8
        }
        catch {
            # Logging is optional and must never stop activity reporting.
        }
    }
}

function Find-CodexExecutable {
    $command = Get-Command codex -ErrorAction SilentlyContinue
    if ($null -ne $command -and (Test-Path -LiteralPath $command.Source)) {
        return $command.Source
    }

    $desktopRoot = Join-Path $env:LOCALAPPDATA 'OpenAI\Codex\bin'
    if (Test-Path -LiteralPath $desktopRoot) {
        $desktop = Get-ChildItem -LiteralPath $desktopRoot -Recurse -Filter 'codex.exe' -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $desktop) { return $desktop.FullName }
    }

    $extensionsRoot = Join-Path $env:USERPROFILE '.vscode\extensions'
    if (Test-Path -LiteralPath $extensionsRoot) {
        $extension = Get-ChildItem -LiteralPath $extensionsRoot -Directory -Filter 'openai.chatgpt-*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($null -ne $extension) {
            $candidate = Join-Path $extension.FullName 'bin\windows-x86_64\codex.exe'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    return $null
}

function Send-AppServerLine(
    [Diagnostics.Process]$Process,
    [hashtable]$Message
) {
    $Process.StandardInput.WriteLine(($Message | ConvertTo-Json -Compress -Depth 10))
    $Process.StandardInput.Flush()
}

function Read-AppServerResponse(
    [Diagnostics.Process]$Process,
    [int]$Id,
    [int]$TimeoutSeconds = 3
) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $pendingRead = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq $pendingRead) {
            $pendingRead = $Process.StandardOutput.ReadLineAsync()
        }
        if (-not $pendingRead.Wait(200)) { continue }
        $line = $pendingRead.Result
        $pendingRead = $null
        if ($null -eq $line) { return $null }
        try {
            $message = $line | ConvertFrom-Json
            if ($message.id -eq $Id) { return $message }
        }
        catch {
            # Ignore diagnostics accidentally written to the protocol stream.
        }
    }
    return $null
}

function Stop-LimitAppServer {
    $process = $script:limitServer
    $script:limitServer = $null
    $script:limitServerRequestId = 1
    if ($null -eq $process) { return }
    try {
        if (-not $process.HasExited) {
            $process.Kill()
            [void]$process.WaitForExit(1000)
        }
    }
    catch { # Already exited, or failed before Start completed.
    }
    finally { $process.Dispose() }
}

function Get-AccountFingerprint([string]$Email) {
    if ([string]::IsNullOrWhiteSpace($Email)) { return '00000000' }
    [uint64]$hash = 2166136261
    foreach ($unit in $Email.Trim().ToLowerInvariant().ToCharArray()) {
        $hash = (($hash -bxor [uint64][char]$unit) * [uint64]16777619) -band [uint64]4294967295
    }
    return $hash.ToString('X8')
}

function Read-QuotaAccountFingerprint([Diagnostics.Process]$Process) {
    $script:limitServerRequestId++
    $requestId = $script:limitServerRequestId
    Send-AppServerLine $Process @{
        method = 'account/read'; id = $requestId; params = @{ refreshToken = $false }
    }
    $response = Read-AppServerResponse $Process $requestId
    if ($null -eq $response -or $null -ne $response.error) { throw 'Account verification failed.' }
    return Get-AccountFingerprint $response.result.account.email
}

function Get-RemainingPercent {
    # The worker calls this serially. Keep one initialized stdio connection;
    # never retain a timed-out read or reuse a stream after protocol failure.
    $script:remainingPercent = -1
    $script:quotaFingerprint = '00000000'
    $script:codexResetQuota = $null
    if ([DateTime]::UtcNow -lt $script:limitServerRetryAt) { return -1 }
    try {
        if ($null -eq $script:limitServer) {
            $executable = Find-CodexExecutable
            if ([string]::IsNullOrWhiteSpace($executable)) { throw 'Codex executable unavailable.' }
            $processInfo = [Diagnostics.ProcessStartInfo]::new()
            $processInfo.FileName = $executable
            $processInfo.Arguments = 'app-server --stdio --disable shell_tool --disable unified_exec'
            $processInfo.UseShellExecute = $false
            $processInfo.CreateNoWindow = $true
            $processInfo.RedirectStandardInput = $true
            $processInfo.RedirectStandardOutput = $true
            $processInfo.RedirectStandardError = $true
            $script:limitServer = [Diagnostics.Process]::new()
            $script:limitServer.StartInfo = $processInfo
            if (-not $script:limitServer.Start()) { throw 'App-server did not start.' }
            # Drain and discard diagnostics as they arrive, without accumulating
            # the lifetime stderr transcript in a ReadToEndAsync task.
            $script:limitServer.BeginErrorReadLine()
            Send-AppServerLine $script:limitServer @{
                method = 'initialize'
                id = 1
                params = @{
                    clientInfo = @{
                        name = 'led_forge_lan_agent'
                        title = 'LED Forge LAN Agent'
                        version = '1.0.0'
                    }
                    capabilities = @{ experimentalApi = $true }
                }
            }
            $initialized = Read-AppServerResponse $script:limitServer 1
            if ($null -eq $initialized -or $null -ne $initialized.error) { throw 'App-server initialization failed.' }
            Send-AppServerLine $script:limitServer @{ method = 'initialized'; params = @{} }
        }
        $process = $script:limitServer
        if ($process.HasExited) { throw 'App-server exited.' }
        # Verify the identity reported by this server, on both sides of the
        # quota request. This cannot force an older server to reload cached auth.
        $beforeFingerprint = Read-QuotaAccountFingerprint $process
        if ($beforeFingerprint -ne $AccountFingerprint) { return -1 }
        $script:limitServerRequestId++
        $requestId = $script:limitServerRequestId
        Send-AppServerLine $process @{
            method = 'account/rateLimits/read'
            id = $requestId
            params = @{}
        }
        $response = Read-AppServerResponse $process $requestId
        if ($null -eq $response -or $null -ne $response.error) { throw 'App-server limits request failed.' }

        $observedAt = [DateTimeOffset]::UtcNow
        $afterFingerprint = Read-QuotaAccountFingerprint $process
        if ($afterFingerprint -ne $AccountFingerprint) { return -1 }

        $limits = @()
        $byId = $response.result.rateLimitsByLimitId
        if ($null -ne $byId) {
            $limits = @($byId.PSObject.Properties | ForEach-Object { $_.Value })
        }
        elseif ($null -ne $response.result.rateLimits) {
            $limits = @($response.result.rateLimits)
        }
        $maximumUsed = $null
        $resetWindow = $null
        $resetWindowMinutes = 0
        $resetWindowUsed = $null
        foreach ($limit in $limits) {
            foreach ($window in @($limit.primary, $limit.secondary)) {
                if ($null -eq $window -or $null -eq $window.usedPercent) { continue }
                $used = [double]$window.usedPercent
                if ($null -eq $maximumUsed -or $used -gt $maximumUsed) {
                    $maximumUsed = $used
                }
                # Prefer an explicit five-hour window. Some Codex accounts
                # only expose a weekly window, which has its own observed reset.
                $duration = $window.windowDurationMins
                if ($null -eq $duration) { $duration = $window.window_duration_mins }
                if ([string]$duration -cnotin @('300', '10080') -or
                    [double]::IsNaN($used) -or $used -lt 0 -or $used -gt 100) { continue }
                $minutes = [int]$duration
                if ($null -eq $resetWindow -or $minutes -lt $resetWindowMinutes -or
                    ($minutes -eq $resetWindowMinutes -and $used -gt $resetWindowUsed)) {
                    $resetWindow = $window
                    $resetWindowMinutes = $minutes
                    $resetWindowUsed = $used
                }
            }
        }
        if ($null -eq $maximumUsed) { return -1 }
        $remaining = [Math]::Max(0.0, [Math]::Min(100.0, 100.0 - $maximumUsed))
        $script:quotaFingerprint = $AccountFingerprint.ToUpperInvariant()
        # A missing or invalid reset clears the previous window on the controller.
        $reset = $resetWindow.resetsAt
        if ($null -eq $reset) { $reset = $resetWindow.resets_at }
        if ($null -eq $reset) { $reset = $resetWindow.reset_at }
        if ($AccountFingerprint -ieq (Get-AccountFingerprint 'marketing@tiras.ua')) {
            $script:codexResetQuota = @{ Fingerprint=$script:quotaFingerprint; ResetEpoch=$reset; FetchedAt=$observedAt }
        }
        return [Math]::Round($remaining, 2)
    }
    catch {
        Stop-LimitAppServer
        $script:limitServerRetryAt = [DateTime]::UtcNow.AddSeconds(60)
        return -1
    }
}

function Update-AccountRemaining([double]$LatestRemaining) {
    # Shared account usage cannot identify which local client did the work.
    $script:remainingPercent = [int][Math]::Round($LatestRemaining)
}

function Get-RecentSessionFiles {
    $result = @{}
    if (-not (Test-Path -LiteralPath $SessionsRoot -PathType Container)) { return }
    $now = Get-Date
    foreach ($day in @((Get-Date), (Get-Date).AddDays(-1))) {
        $directory = Join-Path $SessionsRoot $day.ToString('yyyy')
        $directory = Join-Path $directory $day.ToString('MM')
        $directory = Join-Path $directory $day.ToString('dd')
        if (-not (Test-Path -LiteralPath $directory)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $directory -Filter '*.jsonl' -File -ErrorAction SilentlyContinue) {
            if ($file.LastWriteTime -gt $now.AddMinutes(-10) -or $offsets.ContainsKey($file.FullName)) {
                $result[$file.FullName] = $file
            }
        }
    }
    foreach ($path in @($offsets.Keys)) {
        if ($result.ContainsKey($path)) { continue }
        $file = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
        if ($null -ne $file) { $result[$path] = $file }
    }
    if ($now -ge $nextSessionDiscovery) {
        $script:nextSessionDiscovery = $now.AddSeconds(15)
        foreach ($file in Get-ChildItem -LiteralPath $SessionsRoot -Filter '*.jsonl' -File -Recurse -ErrorAction SilentlyContinue) {
            if ($file.LastWriteTime -gt $now.AddMinutes(-10)) {
                $result[$file.FullName] = $file
            }
        }
    }
    return $result.Values
}

function Apply-SessionLine(
    [string]$Path,
    [string]$Line,
    [bool]$AllowTokenBoost
) {
    if ($Line -notlike '*"type":"task_started"*' -and
        $Line -notlike '*"type":"task_complete"*' -and
        $Line -notlike '*"type":"turn_aborted"*' -and
        $Line -notlike '*"type":"token_count"*') {
        return
    }
    try {
        $event = $Line | ConvertFrom-Json
        if ($event.type -ne 'event_msg') { return }
        $eventType = [string]$event.payload.type
        switch ($eventType) {
            'task_started' {
                $sessionActive[$Path] = $true
            }
            'task_complete' {
                $sessionActive[$Path] = $false
            }
            'turn_aborted' {
                $sessionActive[$Path] = $false
            }
            'token_count' {
                if (-not $AllowTokenBoost) { return }
                $usage = $event.payload.info.last_token_usage
                $workTokens = [double]$usage.output_tokens + [double]$usage.reasoning_output_tokens
                if ($workTokens -le 0) { return }
                $level = if ($workTokens -ge 2000) {
                    0.90
                }
                elseif ($workTokens -ge 1000) {
                    0.70
                }
                elseif ($workTokens -ge 250) {
                    0.50
                }
                elseif ($workTokens -ge 50) {
                    0.30
                }
                else {
                    0.10
                }
                $script:boostActivity = [Math]::Max($script:boostActivity, $level)
                $script:boostUntil = (Get-Date).AddSeconds(8)
            }
        }
    }
    catch {
        # A concurrently written final line will be retried after the next append.
    }
}

function Initialize-Session([System.IO.FileInfo]$File) {
    $path = $File.FullName
    $offsets[$path] = $File.Length
    $remainders[$path] = ''
    $sessionActive[$path] = $false
    $sessionLastWrite[$path] = $File.LastWriteTime

    $stream = [System.IO.File]::Open(
        $path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    )
    try {
        $tailBytes = 4MB
        $start = [Math]::Max(0, $stream.Length - $tailBytes)
        [void]$stream.Seek($start, [System.IO.SeekOrigin]::Begin)
        $reader = [System.IO.StreamReader]::new($stream)
        try {
            if ($start -gt 0) { [void]$reader.ReadLine() }
            while (-not $reader.EndOfStream) {
                Apply-SessionLine $path $reader.ReadLine() $false
            }
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Read-NewSessionContent([System.IO.FileInfo]$File) {
    $path = $File.FullName
    $length = $File.Length
    if (-not $offsets.ContainsKey($path)) {
        Initialize-Session $File
        return
    }
    $offset = [long]$offsets[$path]
    if ($length -lt $offset) {
        $offset = 0
        $remainders[$path] = ''
        $sessionActive[$path] = $false
    }
    if ($length -eq $offset) { return }

    $stream = [System.IO.File]::Open(
        $path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    )
    try {
        [void]$stream.Seek($offset, [System.IO.SeekOrigin]::Begin)
        $reader = [System.IO.StreamReader]::new($stream)
        try {
            $text = $reader.ReadToEnd()
            $offsets[$path] = $stream.Position
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }

    if ([string]::IsNullOrEmpty($text)) { return }
    $sessionLastWrite[$path] = $File.LastWriteTime
    $combined = [string]$remainders[$path] + $text
    $lines = $combined -split "`n"
    $remainders[$path] = $lines[-1]
    for ($index = 0; $index -lt $lines.Count - 1; $index++) {
        Apply-SessionLine $path $lines[$index].TrimEnd("`r") $true
    }
}

function Get-CurrentActivity {
    $now = Get-Date
    $activeCount = 0
    foreach ($path in @($sessionActive.Keys)) {
        $lastWrite = $sessionLastWrite[$path]
        if ($sessionActive[$path] -and
            $null -ne $lastWrite -and
            $now - $lastWrite -lt [TimeSpan]::FromHours(2)) {
            $activeCount++
        }
    }
    $taskActivity = if ($activeCount -gt 0) {
        (2 * [Math]::Min($activeCount, 5) - 1) / 10.0
    } else { 0.0 }
    if ($now -ge $boostUntil) {
        $script:boostActivity = 0.0
    }
    $script:codexActivity = [Math]::Max($taskActivity, $script:boostActivity)
    $script:claudeActivity = Get-ClaudeActivity
    return [Math]::Max($script:codexActivity, $script:claudeActivity)
}


# Activity reads only transcript events. Never emit transcript contents or paths.
function Apply-ClaudeActivityLine($State, [string]$Line, [DateTime]$WrittenAt) {
    try { $record = $Line | ConvertFrom-Json } catch { return }
    $kind = [string]$record.type
    $message = $record.message
    $event = $record.event
    $stop = [string]$message.stop_reason
    if ($kind -eq 'stream_event') {
        $kind = [string]$event.type
        $stop = [string]$event.delta.stop_reason
        if (-not $stop) { $stop = [string]$event.message.stop_reason }
    }
    if (-not $stop) { $stop = [string]$record.stop_reason }
    $finalOutput = $kind -eq 'assistant' -and $stop -and $stop -ne 'tool_use' -and
        ([double]$message.usage.output_tokens -gt 0 -or @($message.content).Count -gt 0)
    if (($stop -and $stop -ne 'tool_use' -and -not $finalOutput) -or
        $kind -in @('message_stop', 'end_turn', 'result', 'turn_aborted', 'session_end', 'stop') -or
        ($kind -eq 'system' -and $record.subtype -eq 'turn_duration')) {
        $State.Active = $false
        return
    }
    if ($kind -eq 'user') {
        # Tool results continue a tool-use turn. Typed user input never starts activity.
        $toolResult = @($message.content | Where-Object { $_.type -eq 'tool_result' }).Count -gt 0
        if (-not $toolResult) { $State.Active = $false; $State.CompletedAt = [DateTime]::MinValue }
        return
    }
    if ($kind -ne 'assistant' -and $kind -ne 'content_block_delta' -and
        -not ($kind -eq 'message_delta' -and $null -ne $event.usage.output_tokens)) { return }
    $observedAt = $WrittenAt
    if ($null -ne $record.timestamp) {
        $stamp = [DateTimeOffset]::MinValue
        if ([DateTimeOffset]::TryParse([string]$record.timestamp, [ref]$stamp)) {
            if ($stamp.UtcDateTime -lt $observedAt) { $observedAt = $stamp.UtcDateTime }
        }
    }
    $State.Active = -not $finalOutput
    if ($finalOutput) { $State.CompletedAt = $observedAt }
    $State.OutputAt = $observedAt
}

function Find-ClaudeActivityFiles([DateTime]$Now) {
    if ($Now -lt $script:nextClaudeActivityDiscovery) { return }
    $script:nextClaudeActivityDiscovery = $Now.AddSeconds(15)
    foreach ($path in @($claudeActivityState.Keys)) {
        $file = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
        if ($null -eq $file -or $file.LastWriteTimeUtc -lt $Now.AddDays(-1)) {
            $claudeActivityState.Remove($path)
        }
    }
    # Resume a depth-first walk, with fixed entry/time/depth budgets. Never recurse
    # the complete project history on a telemetry tick. At most eight enumerators live.
    $deadline = [DateTime]::UtcNow.AddMilliseconds(100)
    try {
        if ($claudeDiscoveryStack.Count -eq 0) {
            if (-not [IO.Directory]::Exists($ClaudeRoot)) { return }
            $claudeDiscoveryStack.Add([IO.Directory]::EnumerateFileSystemEntries($ClaudeRoot).GetEnumerator())
        }
        for ($visited = 0; $visited -lt 512 -and $claudeDiscoveryStack.Count -gt 0 -and
            [DateTime]::UtcNow -lt $deadline; $visited++) {
            $iterator = $claudeDiscoveryStack[$claudeDiscoveryStack.Count - 1]
            if (-not $iterator.MoveNext()) {
                $iterator.Dispose()
                $claudeDiscoveryStack.RemoveAt($claudeDiscoveryStack.Count - 1)
                continue
            }
            $path = $iterator.Current
            $attributes = [IO.File]::GetAttributes($path)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                if ($claudeDiscoveryStack.Count -lt 8) {
                    $claudeDiscoveryStack.Add([IO.Directory]::EnumerateFileSystemEntries($path).GetEnumerator())
                }
                continue
            }
            if ([IO.Path]::GetExtension($path) -ne '.jsonl') { continue }
            $file = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
            if ($null -eq $file -or $file.LastWriteTimeUtc -lt $Now.AddDays(-1) -or
                $claudeActivityState.ContainsKey($path)) { continue }
            if ($claudeActivityState.Count -ge 32) {
                $oldest = $claudeActivityState.Keys | Sort-Object { $claudeActivityState[$_].WrittenAt } | Select-Object -First 1
                if ($file.LastWriteTimeUtc -le $claudeActivityState[$oldest].WrittenAt) { continue }
                $claudeActivityState.Remove($oldest)
            }
            $claudeActivityState[$path] = @{
                Offset = -1L; Tail = ''; Discard = $false; Active = $false; WindowSeconds = 15
                OutputAt = [DateTime]::MinValue; CompletedAt = [DateTime]::MinValue; WrittenAt = $file.LastWriteTimeUtc
                CreatedAt = $file.CreationTimeUtc; Decoder = [Text.Encoding]::UTF8.GetDecoder()
            }
        }
    }
    catch {
        # An inaccessible/deleted directory must not interrupt telemetry or log content.
        foreach ($iterator in $claudeDiscoveryStack) { $iterator.Dispose() }
        $claudeDiscoveryStack.Clear()
    }
}

function Read-ClaudeActivityFile([string]$Path, $State) {
    $stream = $null
    try {
        $file = Get-Item -LiteralPath $Path -ErrorAction Stop
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $length = $stream.Length
        if ($State.Offset -lt 0 -or $length -lt $State.Offset -or
            $file.CreationTimeUtc -ne $State.CreatedAt -or
            ($length -eq $State.Offset -and $file.LastWriteTimeUtc -ne $State.WrittenAt)) {
            $State.Offset = [Math]::Max(0L, $length - 256KB)
            $State.Tail = ''; $State.Discard = $State.Offset -gt 0
            $State.Active = $false; $State.CompletedAt = [DateTime]::MinValue; $State.Decoder.Reset()
            $State.CreatedAt = $file.CreationTimeUtc
        }
        $State.WrittenAt = $file.LastWriteTimeUtc
        if ($length -le $State.Offset) { return }
        # Skip a backlog beyond the per-file budget, preserving only the newest tail.
        if ($length - $State.Offset -gt 256KB) {
            $State.Offset = $length - 256KB
            $State.Tail = ''; $State.Discard = $true; $State.Active = $false; $State.CompletedAt = [DateTime]::MinValue
            $State.Decoder.Reset()
        }
        [void]$stream.Seek($State.Offset, [IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new([int][Math]::Min(256KB, $length - $State.Offset))
        $count = $stream.Read($buffer, 0, $buffer.Length)
        $State.Offset += $count
        $chars = [char[]]::new([Text.Encoding]::UTF8.GetMaxCharCount($count))
        $charCount = $State.Decoder.GetChars($buffer, 0, $count, $chars, 0, $false)
        $text = $State.Tail + [string]::new($chars, 0, $charCount)
        $State.Tail = ''
        $start = 0
        while (($end = $text.IndexOf("`n", $start)) -ge 0) {
            if (-not $State.Discard -and $end - $start -le 256KB) {
                Apply-ClaudeActivityLine $State $text.Substring($start, $end - $start).TrimEnd("`r") $State.WrittenAt
            }
            $State.Discard = $false
            $start = $end + 1
        }
        if ($text.Length - $start -gt 256KB) { $State.Discard = $true }
        if (-not $State.Discard) { $State.Tail = $text.Substring($start) }
    }
    catch {
        # Locked, rotating or missing transcripts are retried, without payload logging.
    }
    finally { if ($null -ne $stream) { $stream.Dispose() } }
}

function Get-ClaudeActivity {
    $now = [DateTime]::UtcNow
    Find-ClaudeActivityFiles $now
    $activeCount = 0
    foreach ($path in @($claudeActivityState.Keys)) {
        $state = $claudeActivityState[$path]
        Read-ClaudeActivityFile $path $state
        $working = $state.Active -and $now - $state.OutputAt -le [TimeSpan]::FromSeconds(15)
        $completed = $null -ne $state.CompletedAt -and $now - $state.CompletedAt -le [TimeSpan]::FromSeconds(4)
        if (($working -or $completed) -and
            $now - $state.WrittenAt -le [TimeSpan]::FromSeconds(15)) { $activeCount++ }
    }
    if ($activeCount -eq 0) { return 0.0 }
    return (2 * [Math]::Min($activeCount, 5) - 1) / 10.0
}


# Claude credentials remain inside this independent worker and never enter telemetry.
function ConvertTo-ClaudeQuota($Usage, [DateTimeOffset]$Now = [DateTimeOffset]::UtcNow) {
    $window = $Usage.five_hour
    if ($null -eq $window -or $null -eq $window.utilization -or
        $window.utilization -is [string] -or $window.utilization -is [bool]) { return $null }
    $used = 0.0
    if (-not [double]::TryParse([string]$window.utilization,
        [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$used) -or
        [double]::IsNaN($used) -or [double]::IsInfinity($used) -or $used -lt 0 -or $used -gt 100) { return $null }
    # An unused five-hour window explicitly reports 0 usage with no reset yet.
    # This is known 100% availability, not a malformed or unknown balance.
    # Keep the reset unknown; observation freshness below supplies its short TTL.
    if ($null -eq $window.resets_at -and $used -eq 0) {
        return [pscustomobject]@{ Remaining = 100; ResetAt = $null }
    }
    $reset = [DateTimeOffset]::MinValue
    if ($window.resets_at -is [DateTimeOffset]) { $reset = $window.resets_at }
    elseif ($window.resets_at -is [DateTime] -and $window.resets_at.Kind -ne [DateTimeKind]::Unspecified) {
        $reset = [DateTimeOffset]$window.resets_at
    }
    elseif ([string]$window.resets_at -notmatch '(Z|[+-]\d{2}:\d{2})$' -or
        -not [DateTimeOffset]::TryParse([string]$window.resets_at,
            [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$reset)) { return $null }
    if ($reset -le $Now -or $reset -gt $Now.AddHours(5).AddMinutes(1)) { return $null }
    [pscustomobject]@{ Remaining = [int][Math]::Floor(100 - $used); ResetAt = $reset }
}

function Initialize-ClaudeDesktopCrypto {
    if ('CodexLanAgent.ClaudeDesktopCache' -as [type]) { return }
    $source = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

namespace CodexLanAgent {
    public sealed class DesktopCredential {
        public string accessToken { get; set; }
        public long expiresAt { get; set; }
        public string Source { get { return "desktop"; } }
    }

    public static class ClaudeDesktopCache {
        const int MaxFile = 4 * 1024 * 1024;
        const int MaxRoots = 32;
        const string Client = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

        [StructLayout(LayoutKind.Sequential)]
        struct AuthInfo {
            public int size, version;
            public IntPtr nonce;
            public int nonceLength;
            public IntPtr authData;
            public int authDataLength;
            public IntPtr tag;
            public int tagLength;
            public IntPtr macContext;
            public int macContextLength, aadLength;
            public ulong dataLength;
            public int flags;
        }

        [DllImport("bcrypt.dll", CharSet = CharSet.Unicode)]
        static extern int BCryptOpenAlgorithmProvider(out IntPtr handle, string algorithm, string provider, int flags);
        [DllImport("bcrypt.dll", CharSet = CharSet.Unicode)]
        static extern int BCryptSetProperty(IntPtr handle, string property, byte[] value, int length, int flags);
        [DllImport("bcrypt.dll", CharSet = CharSet.Unicode)]
        static extern int BCryptGetProperty(IntPtr handle, string property, byte[] value, int length, out int written, int flags);
        [DllImport("bcrypt.dll")]
        static extern int BCryptGenerateSymmetricKey(IntPtr algorithm, out IntPtr key, IntPtr keyObject, int objectLength, byte[] secret, int secretLength, int flags);
        [DllImport("bcrypt.dll")]
        static extern int BCryptDecrypt(IntPtr key, byte[] input, int inputLength, ref AuthInfo info, IntPtr iv, int ivLength, byte[] output, int outputLength, out int written, int flags);
        [DllImport("bcrypt.dll")]
        static extern int BCryptDestroyKey(IntPtr key);
        [DllImport("bcrypt.dll")]
        static extern int BCryptCloseAlgorithmProvider(IntPtr algorithm, int flags);

        static void Check(int status) {
            if (status != 0) throw new CryptographicException("Desktop cache cryptography failed.");
        }
        static void Clear(byte[] bytes) {
            if (bytes != null) Array.Clear(bytes, 0, bytes.Length);
        }

        public static byte[] Decrypt(byte[] keyBytes, byte[] blob) {
            if (keyBytes == null || keyBytes.Length != 32 || blob == null || blob.Length < 31 || blob.Length > MaxFile ||
                blob[0] != (byte)'v' || blob[1] != (byte)'1' || blob[2] != (byte)'0')
                throw new CryptographicException("Unsupported Desktop cache format.");
            IntPtr algorithm = IntPtr.Zero, key = IntPtr.Zero;
            byte[] keyObject = null, cipher = null, plain = null;
            byte[] nonce = new byte[12], tag = new byte[16];
            GCHandle noncePin = new GCHandle(), tagPin = new GCHandle(), keyObjectPin = new GCHandle();
            try {
                Check(BCryptOpenAlgorithmProvider(out algorithm, "AES", null, 0));
                byte[] mode = Encoding.Unicode.GetBytes("ChainingModeGCM\0");
                Check(BCryptSetProperty(algorithm, "ChainingMode", mode, mode.Length, 0));
                byte[] size = new byte[4];
                int written;
                Check(BCryptGetProperty(algorithm, "ObjectLength", size, size.Length, out written, 0));
                int objectLength = BitConverter.ToInt32(size, 0);
                if (objectLength <= 0 || objectLength > 65536) throw new CryptographicException();
                keyObject = new byte[objectLength];
                keyObjectPin = GCHandle.Alloc(keyObject, GCHandleType.Pinned);
                Check(BCryptGenerateSymmetricKey(algorithm, out key, keyObjectPin.AddrOfPinnedObject(), keyObject.Length, keyBytes, keyBytes.Length, 0));
                Buffer.BlockCopy(blob, 3, nonce, 0, 12);
                Buffer.BlockCopy(blob, blob.Length - 16, tag, 0, 16);
                cipher = new byte[blob.Length - 31];
                plain = new byte[cipher.Length];
                Buffer.BlockCopy(blob, 15, cipher, 0, cipher.Length);
                noncePin = GCHandle.Alloc(nonce, GCHandleType.Pinned);
                tagPin = GCHandle.Alloc(tag, GCHandleType.Pinned);
                AuthInfo info = new AuthInfo();
                info.size = Marshal.SizeOf(typeof(AuthInfo));
                info.version = 1;
                info.nonce = noncePin.AddrOfPinnedObject();
                info.nonceLength = nonce.Length;
                info.tag = tagPin.AddrOfPinnedObject();
                info.tagLength = tag.Length;
                Check(BCryptDecrypt(key, cipher, cipher.Length, ref info, IntPtr.Zero, 0, plain, plain.Length, out written, 0));
                if (written != plain.Length) throw new CryptographicException();
                byte[] result = plain;
                plain = null;
                return result;
            } finally {
                if (key != IntPtr.Zero) BCryptDestroyKey(key);
                Clear(keyObject);
                if (keyObjectPin.IsAllocated) keyObjectPin.Free();
                if (algorithm != IntPtr.Zero) BCryptCloseAlgorithmProvider(algorithm, 0);
                if (noncePin.IsAllocated) noncePin.Free();
                if (tagPin.IsAllocated) tagPin.Free();
                Clear(cipher); Clear(plain); Clear(nonce); Clear(tag);
            }
        }

        static bool SafePath(string path) {
            if (String.IsNullOrEmpty(path)) return false;
            string current = Path.GetFullPath(path);
            while (!String.IsNullOrEmpty(current)) {
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) return false;
                current = Path.GetDirectoryName(current);
            }
            return true;
        }

        static Dictionary<string, object> ReadJson(string path) {
            if (!SafePath(path)) return null;
            byte[] bytes = null;
            try {
                using (FileStream stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
                    long length = stream.Length;
                    if (length <= 0 || length > MaxFile) return null;
                    bytes = new byte[(int)length];
                    int count = 0;
                    while (count < bytes.Length) {
                        int read = stream.Read(bytes, count, bytes.Length - count);
                        if (read == 0) return null;
                        count += read;
                    }
                    if (stream.ReadByte() != -1 || !SafePath(path)) return null;
                }
                return Json(new UTF8Encoding(false, true).GetString(bytes).TrimStart('\uFEFF'));
            } finally { Clear(bytes); }
        }

        static Dictionary<string, object> Json(string text) {
            JavaScriptSerializer serializer = new JavaScriptSerializer();
            serializer.MaxJsonLength = MaxFile;
            serializer.RecursionLimit = 32;
            return serializer.DeserializeObject(text) as Dictionary<string, object>;
        }
        static object Get(Dictionary<string, object> value, string name) {
            object result;
            return value != null && value.TryGetValue(name, out result) ? result : null;
        }
        static bool IdentityPart(string value) {
            return !String.IsNullOrWhiteSpace(value) && value.Length <= 256 &&
                value.IndexOfAny(new char[] { '|', ':', '\r', '\n', '\0' }) < 0;
        }

        static DesktopCredential ReadRoot(string root, string account, string organization, long now) {
            byte[] encryptedKey = null, protectedKey = null, key = null, blob = null, plain = null;
            try {
                if (!SafePath(root)) return null;
                Dictionary<string, object> config = ReadJson(Path.Combine(root, "config.json"));
                if (!String.Equals(Get(config, "lastKnownAccountUuid") as string, account, StringComparison.Ordinal)) return null;
                string encoded = Get(config, "oauth:tokenCacheV2") as string;
                if (String.IsNullOrEmpty(encoded)) return null;
                Dictionary<string, object> state = ReadJson(Path.Combine(root, "Local State"));
                string encodedKey = Get(Get(state, "os_crypt") as Dictionary<string, object>, "encrypted_key") as string;
                if (String.IsNullOrEmpty(encodedKey)) return null;
                encryptedKey = Convert.FromBase64String(encodedKey);
                if (encryptedKey.Length <= 5 || encryptedKey[0] != 'D' || encryptedKey[1] != 'P' ||
                    encryptedKey[2] != 'A' || encryptedKey[3] != 'P' || encryptedKey[4] != 'I') return null;
                protectedKey = new byte[encryptedKey.Length - 5];
                Buffer.BlockCopy(encryptedKey, 5, protectedKey, 0, protectedKey.Length);
                key = ProtectedData.Unprotect(protectedKey, null, DataProtectionScope.CurrentUser);
                if (key.Length != 32) return null;
                blob = Convert.FromBase64String(encoded);
                plain = Decrypt(key, blob);
                Dictionary<string, object> cache = Json(new UTF8Encoding(false, true).GetString(plain));
                if (cache == null || cache.Count > 4096) return null;
                string prefix = "acct:" + account + "|" + Client + ":" + organization + ":https://api.anthropic.com:";
                DesktopCredential best = null;
                foreach (KeyValuePair<string, object> entry in cache) {
                    if (!entry.Key.StartsWith(prefix, StringComparison.Ordinal)) continue;
                    Dictionary<string, object> value = entry.Value as Dictionary<string, object>;
                    string token = Get(value, "token") as string;
                    object expiry = Get(value, "expiresAt");
                    if (String.IsNullOrWhiteSpace(token) || token.Length > 65536 ||
                        !(expiry is int || expiry is long || expiry is decimal || expiry is double)) continue;
                    double expires = Convert.ToDouble(expiry, System.Globalization.CultureInfo.InvariantCulture);
                    if (Double.IsNaN(expires) || Double.IsInfinity(expires) || expires <= now || expires > 253402300799999L || expires != Math.Truncate(expires)) continue;
                    if (best == null || expires > best.expiresAt)
                        best = new DesktopCredential { accessToken = token, expiresAt = (long)expires };
                }
                return best;
            } catch { return null; }
            finally { Clear(encryptedKey); Clear(protectedKey); Clear(key); Clear(blob); Clear(plain); }
        }

        public static DesktopCredential Read(string local, string roaming, string account, string organization, string email) {
            if (!IdentityPart(account) || !IdentityPart(organization) || String.IsNullOrWhiteSpace(email) || email.Length > 320) return null;
            List<string> roots = new List<string>();
            try {
                if (!String.IsNullOrEmpty(local)) {
                    string parent = Path.Combine(local, "Claude-Data");
                    if (Directory.Exists(parent) && SafePath(parent)) {
                        foreach (string root in Directory.EnumerateDirectories(parent, "anthropicpbc.claude_*", SearchOption.TopDirectoryOnly)) {
                            if (roots.Count >= MaxRoots) break;
                            roots.Add(root);
                        }
                    }
                }
            } catch { }
            if (!String.IsNullOrEmpty(roaming)) {
                try { roots.Add(Path.Combine(roaming, "Claude")); } catch { }
            }
            DesktopCredential best = null;
            long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            foreach (string root in roots) {
                DesktopCredential candidate = ReadRoot(root, account, organization, now);
                if (candidate != null && (best == null || candidate.expiresAt > best.expiresAt)) best = candidate;
            }
            return best;
        }
    }
}
'@
    Add-Type -TypeDefinition $source -ReferencedAssemblies 'System.dll', 'System.Core.dll', 'System.Security.dll', 'System.Web.Extensions.dll' -ErrorAction Stop
}

function Read-ClaudeDesktopCredential($Identity) {
    try {
        Initialize-ClaudeDesktopCrypto
        if ($null -eq $Identity) { return }
        $credential = [CodexLanAgent.ClaudeDesktopCache]::Read(
            $env:LOCALAPPDATA, $env:APPDATA, [string]$Identity.accountUuid,
            [string]$Identity.organizationUuid, [string]$Identity.emailAddress)
        if ($null -ne $credential) { return $credential }
    }
    catch { # Cache availability and format failures leave credential selection to the caller.
    }
}

function New-ClaudeQuotaFailure {
    param(
        [ValidateSet('expired','refresh','auth','api','identity','network','payload')][string]$Failure,
        [int]$HttpStatus = 0,
        [double]$RetrySeconds = 0
    )
    [pscustomobject]@{ Kind = 'QuotaFailure'; Failure = $Failure; HttpStatus = $HttpStatus; RetrySeconds = $RetrySeconds }
}

function Get-ClaudeHttpFailure($ErrorRecord, [string]$Stage) {
    $status = 0; $retrySeconds = 0.0
    try { $status = [int]$ErrorRecord.Exception.Response.StatusCode } catch { }
    if ($status -eq 429) {
        $retry = ''
        try { $retry = [string]$ErrorRecord.Exception.Response.Headers['Retry-After'] } catch { }
        if (-not $retry) {
            try { $retry = [string]($ErrorRecord.Exception.Response.Headers.GetValues('Retry-After') | Select-Object -First 1) } catch { }
        }
        $date = [DateTimeOffset]::MinValue
        if (-not [double]::TryParse($retry, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$retrySeconds)) {
            if ([DateTimeOffset]::TryParse($retry, [ref]$date)) { $retrySeconds = ($date - [DateTimeOffset]::UtcNow).TotalSeconds }
        }
        if ([double]::IsNaN($retrySeconds) -or [double]::IsInfinity($retrySeconds)) { $retrySeconds = 0 }
        $retrySeconds = [Math]::Max(120.0, [Math]::Min(86400.0, $retrySeconds))
    }
    $failure = $Stage
    if ($status -eq 401 -or $status -eq 403) { $failure = 'auth' }
    elseif ($status -eq 0 -and $Stage -eq 'api') { $failure = 'network' }
    New-ClaudeQuotaFailure $failure $status $retrySeconds
}

function Test-ClaudeCredentialPath([string]$Path) {
    # Reject device/UNC/relative paths and reparse points at every existing component.
    if ($Path -notmatch '\A[A-Za-z]:[\\/]' -or $Path -match '[\x00-\x1f]' -or $Path.Substring(2).Contains(':')) { return $false }
    $fullPath = [IO.Path]::GetFullPath($Path)
    $current = $fullPath
    while ($current) {
        if (([IO.File]::GetAttributes($current) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $false }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
    return [IO.File]::Exists($fullPath)
}

function Read-ClaudeUsableDesktopCredential($Identity) {
    try {
        $grant = Read-ClaudeDesktopCredential $Identity
        $expires = 0.0
        if ($null -ne $grant -and $grant.Source -ceq 'desktop' -and $grant.accessToken -is [string] -and
            -not [string]::IsNullOrWhiteSpace($grant.accessToken) -and
            [double]::TryParse([string]$grant.expiresAt, [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$expires) -and
            -not [double]::IsNaN($expires) -and -not [double]::IsInfinity($expires) -and
            $expires -gt [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) { return $grant }
    }
    catch { # Desktop cache failures are silent; the CLI grant remains a fallback.
    }
}

function Read-ClaudeCredentialText([string]$Path) {
    if (-not (Test-ClaudeCredentialPath $Path)) { throw 'Credential path unavailable.' }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        if ($stream.Length -le 0 -or $stream.Length -gt 4MB) { throw 'Credential document size invalid.' }
        $bytes = [byte[]]::new([int]$stream.Length)
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $read = $stream.Read($bytes, $offset, $bytes.Length - $offset)
            if ($read -eq 0) { throw 'Credential document changed.' }
            $offset += $read
        }
        if ($stream.ReadByte() -ne -1 -or -not (Test-ClaudeCredentialPath $Path)) { throw 'Credential document changed.' }
        return [Text.UTF8Encoding]::new($false, $true).GetString($bytes).TrimStart([char]0xFEFF)
    }
    finally {
        $stream.Dispose()
        if ($null -ne $bytes) { [Array]::Clear($bytes, 0, $bytes.Length) }
    }
}

function Read-ClaudeQuotaIdentity([string]$Text) {
    if ([string]::IsNullOrWhiteSpace($Text) -or $Text.Length -gt 4MB) { throw 'Invalid Claude identity document.' }
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        $document = ConvertFrom-Json -InputObject $Text -AsHashtable -Depth 32
    }
    else {
        Add-Type -AssemblyName System.Web.Extensions -ErrorAction Stop
        $parser = [Web.Script.Serialization.JavaScriptSerializer]::new()
        $parser.MaxJsonLength = 4MB
        $parser.RecursionLimit = 32
        $document = $parser.DeserializeObject($Text)
    }
    $identity = $document['oauthAccount']
    if ($identity -isnot [Collections.IDictionary] -or
        $identity['emailAddress'] -isnot [string] -or
        $identity['accountUuid'] -isnot [string] -or
        $identity['organizationUuid'] -isnot [string]) { throw 'Invalid Claude account identity.' }
    [pscustomobject]@{
        emailAddress = [string]$identity['emailAddress']
        accountUuid = [string]$identity['accountUuid']
        organizationUuid = [string]$identity['organizationUuid']
    }
}

function Copy-ClaudeVerifiedProfile($Profile, [DateTimeOffset]$Now = [DateTimeOffset]::UtcNow) {
    if ($null -eq $Profile) { return }
    foreach ($name in @('GrantDigest','AccountUuid','OrganizationUuid','EmailAddress')) {
        if ($Profile.$name -isnot [string] -or [string]::IsNullOrWhiteSpace($Profile.$name)) { return }
    }
    if ($Profile.GrantDigest -cnotmatch '\A[0-9A-F]{64}\z' -or $Profile.EmailAddress -cne 'marketing@tiras.ua') { return }
    try {
        $checked = [DateTimeOffset]$Profile.CheckedAt
        $expires = [DateTimeOffset]$Profile.ExpiresAt
        if ($null -eq $Profile.CheckedAt -or $null -eq $Profile.ExpiresAt -or
            $checked -gt $Now -or ($Now - $checked).TotalMinutes -ge 15 -or
            $expires -le $Now -or $expires -gt $checked.AddMinutes(15)) { return }
    } catch { return }
    # Copy only verified metadata. Never retain a grant or extra worker properties.
    [pscustomobject]@{ GrantDigest=$Profile.GrantDigest; AccountUuid=$Profile.AccountUuid
        OrganizationUuid=$Profile.OrganizationUuid; EmailAddress=$Profile.EmailAddress
        CheckedAt=$checked; ExpiresAt=$expires }
}

function Read-ClaudeQuota($VerifiedProfile = $null) {
    $ErrorActionPreference = 'Stop'
    $stage = 'payload'; $desktop = $null; $verified = $null
    try {
        $credentialPath = Join-Path $env:USERPROFILE '.claude/.credentials.json'
        $identity = Read-ClaudeQuotaIdentity (Read-ClaudeCredentialText (Join-Path $env:USERPROFILE '.claude.json'))
        $oauth = (Read-ClaudeCredentialText $credentialPath | ConvertFrom-Json).claudeAiOauth
        if ([string]$identity.emailAddress -cne 'marketing@tiras.ua' -or
            [string]::IsNullOrWhiteSpace([string]$identity.accountUuid) -or
            [string]::IsNullOrWhiteSpace([string]$identity.organizationUuid) -or
            ($null -ne $oauth.PSObject.Properties['organizationUuid'] -and
                $identity.organizationUuid -cne $oauth.organizationUuid)) { return (New-ClaudeQuotaFailure 'identity') }
        $expires = 0.0
        $cliValid = $oauth.accessToken -is [string] -and -not [string]::IsNullOrWhiteSpace($oauth.accessToken) -and
            [double]::TryParse([string]$oauth.expiresAt, [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$expires) -and
            -not [double]::IsNaN($expires) -and -not [double]::IsInfinity($expires) -and
            $expires -gt [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        if ($cliValid) { $accessToken = $oauth.accessToken }
        else {
            $desktop = Read-ClaudeUsableDesktopCredential $identity
            if ($null -eq $desktop) { return (New-ClaudeQuotaFailure 'expired') }
            $accessToken = $desktop.accessToken
        }
        $headers = @{ Authorization = "Bearer $accessToken"; 'anthropic-beta' = 'oauth-2025-04-20' }
        $stage = 'api'
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $digest = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($accessToken))).Replace('-', '') }
        finally { $hasher.Dispose() }
        $grantExpiry = $oauth.expiresAt
        if ($null -ne $desktop) { $grantExpiry = $desktop.expiresAt }
        $verified = Copy-ClaudeVerifiedProfile $VerifiedProfile
        if ($null -ne $verified -and ($verified.GrantDigest -cne $digest -or
            $verified.AccountUuid -cne $identity.accountUuid -or
            $verified.OrganizationUuid -cne $identity.organizationUuid -or
            $verified.EmailAddress -cne $identity.emailAddress -or
            $verified.ExpiresAt.ToUnixTimeMilliseconds() -gt [double]$grantExpiry)) { $verified = $null }
        if ($null -eq $verified) {
            try {
                $profile = Invoke-RestMethod -Uri 'https://api.anthropic.com/api/oauth/profile' -Headers $headers -TimeoutSec 4 -MaximumRedirection 0
            }
            catch {
                $failure = Get-ClaudeHttpFailure $_ 'api'
                if ($failure.HttpStatus -ne 401 -or $null -ne $desktop) { return $failure }
                $desktop = Read-ClaudeUsableDesktopCredential $identity
                if ($null -eq $desktop) { return $failure }
                $accessToken = $desktop.accessToken
                $headers.Authorization = "Bearer $accessToken"
                $profile = Invoke-RestMethod -Uri 'https://api.anthropic.com/api/oauth/profile' -Headers $headers -TimeoutSec 4 -MaximumRedirection 0
                $hasher = [Security.Cryptography.SHA256]::Create()
                try { $digest = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($accessToken))).Replace('-', '') }
                finally { $hasher.Dispose() }
            }
            if ([string]$profile.account.email -cne $identity.emailAddress -or
                $profile.account.uuid -cne $identity.accountUuid -or
                $profile.organization.uuid -cne $identity.organizationUuid) { return (New-ClaudeQuotaFailure 'identity') }
            $checked = [DateTimeOffset]::UtcNow
            $grantExpiry = $oauth.expiresAt
            if ($null -ne $desktop) { $grantExpiry = $desktop.expiresAt }
            $expiresAt = $checked.AddMinutes(15)
            if ([double]$grantExpiry -lt $expiresAt.ToUnixTimeMilliseconds()) {
                $expiresAt = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$grantExpiry)
            }
            $verified = [pscustomobject]@{ GrantDigest=$digest; AccountUuid=$identity.accountUuid
                OrganizationUuid=$identity.organizationUuid; EmailAddress=$identity.emailAddress
                CheckedAt=$checked; ExpiresAt=$expiresAt }
        }
        $usage = Invoke-RestMethod -Uri 'https://api.anthropic.com/api/oauth/usage' -Headers $headers -TimeoutSec 4 -MaximumRedirection 0
        $quota = ConvertTo-ClaudeQuota $usage
        if ($null -ne $quota) {
            $source = 'cli'; if ($null -ne $desktop) { $source = 'desktop' }
            [pscustomobject]@{ Kind = 'QuotaSuccess'; Source = $source; Remaining = $quota.Remaining; ResetAt = $quota.ResetAt
                FetchedAt = [DateTimeOffset]::UtcNow; Fingerprint = (Get-AccountFingerprint 'marketing@tiras.ua'); VerifiedProfile = $verified }
        }
        else { New-ClaudeQuotaFailure 'payload' }
    }
    catch {
        $failure = Get-ClaudeHttpFailure $_ $stage
        if ($failure.HttpStatus -eq 429 -and $null -ne $verified) {
            $failure | Add-Member -NotePropertyName VerifiedProfile -NotePropertyValue $verified
        }
        $failure
    }
    finally {
        $headers = $null; $oauth = $null; $profile = $null; $local = $null
        $accessToken = $null; $desktop = $null
    }
}

function Test-ClaudeQuotaFresh($Quota, [DateTimeOffset]$Now = [DateTimeOffset]::UtcNow) {
    if ($null -eq $Quota -or $Quota.Fingerprint -cne (Get-AccountFingerprint 'marketing@tiras.ua') -or
        $Quota.Remaining -lt 0 -or $Quota.Remaining -gt 100 -or
        $Quota.FetchedAt -isnot [DateTimeOffset] -or $Quota.FetchedAt -gt $Now -or
        ($Now - $Quota.FetchedAt).TotalSeconds -ge 90) { return $false }
    if ($null -eq $Quota.ResetAt) { return $Quota.Remaining -eq 100 }
    return $Quota.ResetAt -is [DateTimeOffset] -and $Quota.ResetAt -gt $Now
}

function New-ClaudeQuotaPoller {
    @{ Worker = $null; Pending = $null; Stopping = $null; Started = [DateTimeOffset]::MinValue
        NextPoll = [DateTimeOffset]::MinValue; Failures = 0; Quota = $null; VerifiedProfile = $null; PollSeconds = 60
        LastFailure = ''; HttpStatus = 0; LastStatus = ''; LastSource = ''; RetrySeconds = 0
        Configured = (Test-Path -LiteralPath (Join-Path $env:USERPROFILE '.claude.json')) }
}

function Stop-ClaudeQuotaWorker($State) {
    if ($null -eq $State.Worker) { return }
    try {
        if ($null -ne $State.Stopping) { $State.Worker.EndStop($State.Stopping) }
        elseif ($null -ne $State.Pending -and -not $State.Pending.IsCompleted) { $State.Worker.Stop() }
        if ($null -ne $State.Pending) { try { [void]$State.Worker.EndInvoke($State.Pending) } catch { } }
    }
    finally {
        $State.Worker.Dispose(); $State.Worker = $null; $State.Pending = $null; $State.Stopping = $null
    }
}

function Set-ClaudePollFailure($State, $Result, [DateTimeOffset]$Now) {
    $failure = 'network'; $httpStatus = 0; $retrySeconds = 0.0
    if ($null -ne $Result -and $Result.Kind -eq 'QuotaFailure' -and
        $Result.Failure -cin @('expired','refresh','auth','api','identity','network','payload')) {
        $failure = $Result.Failure
        if ($Result.HttpStatus -ge 100 -and $Result.HttpStatus -le 599) { $httpStatus = [int]$Result.HttpStatus }
        $retrySeconds = [double]$Result.RetrySeconds
        if ([double]::IsNaN($retrySeconds) -or [double]::IsInfinity($retrySeconds) -or $retrySeconds -lt 0) { $retrySeconds = 0 }
        $retrySeconds = [Math]::Min(86400.0, $retrySeconds)
    }
    $State.Failures = [Math]::Min(7, $State.Failures + 1)
    $delay = [Math]::Min(900, 15 * [Math]::Pow(2, $State.Failures - 1))
    if ($httpStatus -eq 401 -or $httpStatus -eq 403 -or $failure -in @('auth','identity','expired')) { $State.VerifiedProfile = $null }
    if ($httpStatus -eq 429) {
        $State.PollSeconds = [Math]::Min(180, $State.PollSeconds * 2)
        $delay = [Math]::Max($State.PollSeconds, $delay)
    }
    $delay = [Math]::Max($delay, $retrySeconds)
    # DateTimeOffset has a finite range even when an upstream header is malformed.
    $delay = [Math]::Min([double]$delay, ([DateTimeOffset]::MaxValue - $Now).TotalSeconds - 1)
    $State.LastFailure = $failure; $State.HttpStatus = $httpStatus; $State.RetrySeconds = $delay
    $State.NextPoll = $Now.AddSeconds($delay)
    $status = "$failure/$httpStatus"
    if ($State.LastStatus -cne $status) {
        Write-AgentStatus "Claude quota unavailable ($failure, HTTP $httpStatus); retry in $([Math]::Ceiling($delay))s."
        $State.LastStatus = $status
    }
}

function Update-ClaudeQuotaPoller($State, [DateTimeOffset]$Now = [DateTimeOffset]::UtcNow) {
    if ($null -ne $State.Pending) {
        if ($null -ne $State.Stopping) {
            if ($State.Stopping.IsCompleted) { Stop-ClaudeQuotaWorker $State }
        }
        elseif ($State.Pending.IsCompleted) {
            $result = $null
            try { $result = @($State.Worker.EndInvoke($State.Pending)) | Select-Object -Last 1 } catch { }
            $State.Pending = $null
            Stop-ClaudeQuotaWorker $State
            # Only this independent worker supplies cache metadata, never external state/files.
            $State.VerifiedProfile = Copy-ClaudeVerifiedProfile $result.VerifiedProfile $Now
            if ($null -ne $result -and $result.Kind -eq 'QuotaSuccess' -and (Test-ClaudeQuotaFresh $result $Now)) {
                $State.Quota = $result; $State.Configured = $true; $State.Failures = 0
                $source = 'cli'; if ($result.Source -ceq 'desktop') { $source = 'desktop' }
                if (-not $State.LastSource -or $State.LastFailure -or $State.LastSource -cne $source) {
                    $event = 'ready'; if ($State.LastFailure) { $event = 'recovered' }
                    Write-AgentStatus "Claude quota $event (source=$source, remaining=$([int]$result.Remaining)%)."
                }
                $State.LastSource = $source
                $State.LastFailure = ''; $State.HttpStatus = 0; $State.RetrySeconds = 0; $State.LastStatus = 'ok'
                $State.NextPoll = $Now.AddSeconds($State.PollSeconds)
            }
            else { Set-ClaudePollFailure $State $result $Now }
        }
        elseif (($Now - $State.Started).TotalSeconds -ge 20) {
            # BeginStop avoids blocking the heartbeat on cancellation. Do not replace until disposed.
            $State.Stopping = $State.Worker.BeginStop($null, $null)
            Set-ClaudePollFailure $State (New-ClaudeQuotaFailure 'network') $Now
        }
    }
    if ($null -ne $State.Quota -and -not (Test-ClaudeQuotaFresh $State.Quota $Now)) { $State.Quota = $null }
    if ($null -eq $State.Worker -and $Now -ge $State.NextPoll) {
        $State.Worker = [PowerShell]::Create()
        try {
            foreach ($name in @('Get-AccountFingerprint', 'ConvertTo-ClaudeQuota', 'New-ClaudeQuotaFailure',
                'Get-ClaudeHttpFailure', 'Test-ClaudeCredentialPath', 'Read-ClaudeCredentialText',
                'Initialize-ClaudeDesktopCrypto', 'Read-ClaudeDesktopCredential', 'Read-ClaudeUsableDesktopCredential',
                'Read-ClaudeQuotaIdentity', 'Copy-ClaudeVerifiedProfile', 'Read-ClaudeQuota')) {
                [void]$State.Worker.AddScript("function $name { $( (Get-Item "function:$name").Definition ) }")
            }
            [void]$State.Worker.AddScript('param($VerifiedProfile) Read-ClaudeQuota $VerifiedProfile').AddArgument($State.VerifiedProfile)
            $State.Started = $Now
            $State.Pending = $State.Worker.BeginInvoke()
        }
        catch {
            Stop-ClaudeQuotaWorker $State
            Set-ClaudePollFailure $State (New-ClaudeQuotaFailure 'network') $Now
        }
    }
}

function New-ClaudeQuotaPacket($State, [string]$Account, [string]$Client,
    [DateTimeOffset]$Now = [DateTimeOffset]::UtcNow) {
    if ($Account -cnotmatch '\A[0-9A-Fa-f]{8}\z' -or $Client -cnotmatch '\A[0-9A-Fa-f]{8}\z') { throw 'Invalid Claude packet identity.' }
    $remaining = -1; $fingerprint = '00000000'; $validForMs = 0
    $quota = $State.Quota
    if (Test-ClaudeQuotaFresh $quota $Now) {
        $remaining = $quota.Remaining; $fingerprint = $quota.Fingerprint
        $validFor = 90000 - ($Now - $quota.FetchedAt).TotalMilliseconds
        if ($null -ne $quota.ResetAt) { $validFor = [Math]::Min($validFor, ($quota.ResetAt - $Now).TotalMilliseconds) }
        $validForMs = [int][Math]::Floor($validFor)
        if ($validForMs -le 0) { $remaining = -1; $fingerprint = '00000000'; $validForMs = 0 }
    }
    "LFC1,$Account,$Client,$remaining,$fingerprint,$validForMs"
}

# LFR1 is supplemental metadata only; receivers must not refresh quota TTL.
# Reset zero explicitly clears the previous window. Age is measured from the observation,
# never from the heartbeat or HTTP enqueue time.
function New-QuotaResetPacket([string]$Account, [string]$Client, [string]$Provider,
    $Quota, [DateTimeOffset]$Now = [DateTimeOffset]::UtcNow) {
    if ($Account -cnotmatch '\A[0-9A-Fa-f]{8}\z' -or $Client -cnotmatch '\A[0-9A-Fa-f]{8}\z' -or
        $Provider -cnotin @('codex', 'claude') -or $null -eq $Quota -or
        $Quota.Fingerprint -ine $Account -or $Quota.Fingerprint -ine (Get-AccountFingerprint 'marketing@tiras.ua')) { return }
    try {
        if ($Quota.FetchedAt -isnot [DateTimeOffset]) { return }
        $age = ($Now - $Quota.FetchedAt).TotalSeconds
        if ($age -lt 0 -or $age -gt 86400) { return }
        [long]$epoch = 0
        if ($Provider -ceq 'claude') {
            if ($Quota.ResetAt -is [DateTimeOffset]) { $epoch = $Quota.ResetAt.ToUnixTimeSeconds() }
        } elseif ([string]$Quota.ResetEpoch -cmatch '\A[0-9]{10}\z') {
            $epoch = [long]$Quota.ResetEpoch
        }
        if ($epoch -lt 1700000000 -or $epoch -gt 4102444800) { $epoch = 0 }
        $ageSeconds = [long][Math]::Floor($age)
        "LFR1,$Account,$Client,$Provider,$($Quota.Fingerprint),$epoch,$ageSeconds"
    } catch { return }
}

function Update-ClaudeHttpSender($State) {
    if ($null -eq $State -or $null -eq $State.Pending) { return }
    if ($null -ne $State.Stopping) {
        if ($State.Stopping.IsCompleted) { Stop-ClaudeQuotaWorker $State }
    }
    elseif ($State.Pending.IsCompleted) { Stop-ClaudeQuotaWorker $State }
    elseif (([DateTimeOffset]::UtcNow - $State.Started).TotalMilliseconds -ge 750) {
        $State.Stopping = $State.Worker.BeginStop($null, $null)
    }
}

function Start-ClaudeHttpSend($State, [byte[]]$Bytes, [string]$Address, [int]$HttpPort) {
    Update-ClaudeHttpSender $State
    if ($null -eq $State -or $null -ne $State.Worker) { return }
    $State.Worker = [PowerShell]::Create()
    try {
        [void]$State.Worker.AddScript("function Send-DirectHttpPacket { $((Get-Item 'function:Send-DirectHttpPacket').Definition) }")
        [void]$State.Worker.AddScript({
            param($Bytes, $Address, $HttpPort)
            $DirectHost = $Address; $DirectHttpPort = $HttpPort
            [void](Send-DirectHttpPacket $Bytes)
        }).AddArgument($Bytes).AddArgument($Address).AddArgument($HttpPort)
        $State.Started = [DateTimeOffset]::UtcNow
        $State.Pending = $State.Worker.BeginInvoke()
    }
    catch { Stop-ClaudeQuotaWorker $State }
}

function Send-Heartbeat {
    $message = @{
        version = 1
        client = $clientId
        activity = [Math]::Round($script:activity, 2)
        # Legacy bridge has no quota identity; only LFA3 may carry quota.
        remaining = -1
        name = $metadata.Name
        color = $script:effectiveColorHex6
    } | ConvertTo-Json -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($message)
    try { [void]$udp.Send($bytes, $bytes.Length, $CollectorHost, $Port) }
    catch [Net.Sockets.SocketException] { }

    $activityPercent = [int][Math]::Round($script:activity * 100)
    $directMessage = "LFA3,$AccountFingerprint,$clientHash,$activityPercent,$script:remainingPercent,$script:quotaFingerprint,$($metadata.ColorHex6),$($metadata.NameUtf8Hex)"
    $directBytes = [Text.Encoding]::ASCII.GetBytes($directMessage)
    try { [void]$udp.Send($directBytes, $directBytes.Length, $DirectHost, $DirectPort) }
    catch [Net.Sockets.SocketException] { }

    $codexPercent = [int][Math]::Round($script:codexActivity * 100)
    $claudePercent = [int][Math]::Round($script:claudeActivity * 100)
    $originBytes = [Text.Encoding]::ASCII.GetBytes("LFX1,$AccountFingerprint,$clientHash,$codexPercent,$claudePercent")
    try { [void]$udp.Send($originBytes, $originBytes.Length, $DirectHost, $DirectPort) }
    catch [Net.Sockets.SocketException] { }

    $now = Get-Date
    if ($now -ge $script:nextDirectHttpAt) {
        $script:nextDirectHttpAt = $now.AddSeconds(2)
        $ack = Send-DirectHttpPacket $directBytes
        if ($null -ne $ack) {
            Confirm-DirectController $ack "http://${DirectHost}:$DirectHttpPort" $DirectHost
        }
    }
    if ($now -ge $script:nextOriginHttpAt) {
        $script:nextOriginHttpAt = $now.AddSeconds(5)
        Start-ClaudeHttpSend $script:originHttpSender $originBytes $DirectHost $DirectHttpPort
    }
    if ($null -ne $script:claudePoller -and $script:claudePoller.Configured -and
        $now -ge $script:nextClaudeReportAt -and -not $DisableLimitPolling) {
        $script:nextClaudeReportAt = $now.AddSeconds(5)
        $claudeBytes = [Text.Encoding]::ASCII.GetBytes((New-ClaudeQuotaPacket $script:claudePoller $AccountFingerprint $clientHash))
        try { [void]$udp.Send($claudeBytes, $claudeBytes.Length, $DirectHost, $DirectPort) }
        catch [Net.Sockets.SocketException] { }
        Start-ClaudeHttpSend $script:claudeHttpSender $claudeBytes $DirectHost $DirectHttpPort
    }
    if (-not $DisableLimitPolling -and $now -ge $script:nextResetReportAt) {
        $script:nextResetReportAt = $now.AddSeconds(5)
        foreach ($provider in @('codex', 'claude')) {
            $quota = if ($provider -ceq 'codex') { $script:codexResetQuota } else { $script:claudePoller.Quota }
            if ($null -eq $quota) { continue }
            $resetPacket = New-QuotaResetPacket $AccountFingerprint $clientHash $provider $quota
            if (-not $resetPacket) { continue }
            $resetBytes = [Text.Encoding]::ASCII.GetBytes($resetPacket)
            try { [void]$udp.Send($resetBytes, $resetBytes.Length, $DirectHost, $DirectPort) }
            catch [Net.Sockets.SocketException] { }
            Start-ClaudeHttpSend $script:resetHttpSenders[$provider] $resetBytes $DirectHost $DirectHttpPort
        }
    }
    $reportIdentity = ("$AccountFingerprint,$clientHash,$($metadata.ColorHex6),$($metadata.NameUtf8Hex)").ToUpperInvariant()
    # Both providers share one dated slot. RAM acknowledgements leave the cyclic
    # retry schedule intact so a reboot can recover all dates.
    if ($script:codexDailyTokens -or $script:claudeDailyTokens) {
        $today = Get-TokenDay ([DateTimeOffset]$now)
        if ($script:codexDailyTokens -and $script:codexDailyTokens.Today -cne $today) {
            $replay = $script:codexDailyTokens
            $script:codexDailyTokens = Update-DailyReplay $replay $replay.Daily $replay.ReportIdentity 366 ([DateTimeOffset]$now)
        }
        if ($script:claudeDailyTokens -and $script:claudeDailyTokens.Today -cne $today) {
            $replay = $script:claudeDailyTokens
            $script:claudeDailyTokens = Update-DailyReplay $replay $replay.Daily $replay.ReportIdentity 28 ([DateTimeOffset]$now)
        }
    }
    $codexDailyReady = $null -ne $script:codexDailyTokens -and
        $script:codexDailyTokens.ReportIdentity -ceq $reportIdentity -and $script:codexDailyTokens.Daily.Count -gt 0
    $claudeDailyReady = $null -ne $script:claudeDailyTokens -and
        $script:claudeDailyTokens.ReportIdentity -ceq $reportIdentity -and $script:claudeDailyTokens.Daily.Count -gt 0
    $dailyDue = ($codexDailyReady -or $claudeDailyReady) -and $now -ge $script:nextDailyReportAt
    if ($script:weeklyTokensReady -and $script:weeklyTokens.ReportIdentity -ceq $reportIdentity -and
        $now -ge $script:nextWeeklyReportAt -and $script:weeklyTokens.Week -eq (Get-TokenWeek ([DateTimeOffset]$now)) -and
        -not ($dailyDue -and $script:lastTokenSendWasWeekly)) {
        $script:lastTokenSendWasWeekly = $true
        $script:nextWeeklyReportAt = $now.AddSeconds(15)
        $tokenMessage = New-WeeklyTokenPacket $script:weeklyTokens $AccountFingerprint $clientHash $metadata.ColorHex6 $metadata.NameUtf8Hex
        $tokenBytes = [Text.Encoding]::ASCII.GetBytes($tokenMessage)
        try { [void]$udp.Send($tokenBytes, $tokenBytes.Length, $DirectHost, $DirectPort) }
        catch [Net.Sockets.SocketException] { }
        $ack = Send-DirectHttpPacket $tokenBytes
        if ($null -ne $ack) { [void](Test-WeeklyTokenAcknowledgement $ack) }
    } elseif ($dailyDue) {
        $script:lastTokenSendWasWeekly = $false
        $script:nextDailyReportAt = $now.AddSeconds(2)
        if ($claudeDailyReady -and ($script:preferClaudeDaily -or -not $codexDailyReady)) {
            $replay = $script:claudeDailyTokens
            $protocol = 'LFD2'
            $script:preferClaudeDaily = $false
        } else {
            $replay = $script:codexDailyTokens
            $protocol = 'LFD1'
            $script:preferClaudeDaily = $true
        }
        $day = Get-NextDailyReplay $replay
        $dayMessage = New-DailyTokenPacket $day $AccountFingerprint $clientHash $metadata.ColorHex6 $metadata.NameUtf8Hex $protocol
        $dayBytes = [Text.Encoding]::ASCII.GetBytes($dayMessage)
        try { [void]$udp.Send($dayBytes, $dayBytes.Length, $DirectHost, $DirectPort) }
        catch [Net.Sockets.SocketException] { }
        $ack = Send-DirectHttpPacket $dayBytes
        $key = [string]$day.Day
        if ($replay.Pending.Contains($key)) {
            $replay.Pending.Remove($key)
            # Retry an unconfirmed correction behind other changed dates.
            if ($null -eq $ack -or -not (Test-DailyTokenAcknowledgement $ack $day $protocol)) {
                $replay.Pending[$key] = $true
            }
        }
    }
}

function Send-DirectHttpPacket([byte[]]$Bytes) {
    $request = [Net.HttpWebRequest]::Create("http://${DirectHost}:$DirectHttpPort/agent")
    $request.Method = 'POST'
    $request.ContentType = 'text/plain'
    $request.Timeout = 750
    $request.ReadWriteTimeout = 750
    $request.Proxy = $null
    $request.AllowAutoRedirect = $false
    $request.KeepAlive = $false
    $request.ServicePoint.Expect100Continue = $false
    $request.ContentLength = $Bytes.Length
    $deadline = [DateTime]::UtcNow.AddMilliseconds(750)
    try {
        $pending = $request.BeginGetRequestStream($null, $null)
        try {
            if (-not $pending.AsyncWaitHandle.WaitOne([Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds))) { return }
            $stream = $request.EndGetRequestStream($pending)
        }
        finally { $pending.AsyncWaitHandle.Close() }
        try { $stream.Write($Bytes, 0, $Bytes.Length) }
        finally { $stream.Dispose() }
        $pending = $request.BeginGetResponse($null, $null)
        try {
            if (-not $pending.AsyncWaitHandle.WaitOne([Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds))) { return }
            $response = $request.EndGetResponse($pending)
        }
        finally { $pending.AsyncWaitHandle.Close() }
        try {
            $stream = $response.GetResponseStream()
            $buffer = [byte[]]::new(200)
            $count = 0
            while ($count -lt $buffer.Length) {
                $remaining = [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds
                if ($remaining -le 0) { return }
                $stream.ReadTimeout = $remaining
                $read = $stream.Read($buffer, $count, $buffer.Length - $count)
                if ($read -eq 0) { return [Text.Encoding]::ASCII.GetString($buffer, 0, $count) }
                $count += $read
            }
            # ACKs are short. Reject oversized or endlessly streaming responses.
        }
        finally { $response.Dispose() }
    }
    catch [Net.WebException] { }
    catch [IO.IOException] { }
    finally { $request.Abort() }
}

function Test-DailyTokenAcknowledgement([string]$Response, $Day,
    [ValidateSet('LFD1', 'LFD2')][string]$Protocol = 'LFD1') {
    $parts = $Response.Trim() -split ','
    return ($parts.Count -eq 5 -and $parts[0] -ceq "${Protocol}_RAM_ACK" -and
        $parts[1] -cmatch '\A[0-9A-Fa-f]{6}\z' -and $parts[2] -eq $clientHash -and
        ([string]::IsNullOrEmpty($controllerDeviceId) -or $parts[1] -eq $controllerDeviceId) -and
        $parts[3] -ceq [string]$Day.Day -and $parts[4] -ceq [string]$Day.Revision)
}

function Test-WeeklyTokenAcknowledgement([string]$Response) {
    $parts = $Response.Trim() -split ','
    return ($parts.Count -eq 3 -and $parts[0] -ceq 'LFT2_ACK' -and
        $parts[1] -cmatch '\A[0-9A-Fa-f]{6}\z' -and $parts[2] -eq $clientHash -and
        ([string]::IsNullOrEmpty($controllerDeviceId) -or $parts[1] -eq $controllerDeviceId))
}

function Confirm-DirectController([string]$Response, [string]$Endpoint, [string]$HostAddress = '') {
    $parts = $Response.Trim() -split ','
    if (($parts.Count -ne 3 -and $parts.Count -ne 4) -or
        $parts[0] -ne 'LFA2_ACK' -or
        $parts[1] -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or
        (-not [string]::IsNullOrEmpty($controllerDeviceId) -and $parts[1] -ne $controllerDeviceId) -or
        $parts[2] -ne $clientHash) {
        return
    }
    if ($parts.Count -eq 4) {
        if ($parts[3] -cnotmatch '\A[0-9A-Fa-f]{6}\z') { return }
        $script:effectiveColorHex6 = $parts[3].ToUpperInvariant()
    }
    $script:lastDirectAckAt = Get-Date
    $changed = $controllerDeviceId -eq '' -or ($HostAddress -ne '' -and $HostAddress -ne $confirmedDirectHost)
    $script:controllerDeviceId = $parts[1].ToUpperInvariant()
    if ($HostAddress -ne '') { $script:DirectHost = $HostAddress }
    $script:confirmedDirectHost = $DirectHost
    if ($changed) { $script:controllerCacheDirty = $true }
    if ($controllerCacheDirty) {
        try {
            @{ Seed = $configuredDirectHost; Account = $AccountFingerprint; Port = $DirectPort;
                HttpPort = $DirectHttpPort; Host = $DirectHost; Device = $controllerDeviceId } |
                ConvertTo-Json | Set-Content -LiteralPath $ControllerStatePath -Encoding UTF8
            $script:controllerCacheDirty = $false
        }
        catch { # Connectivity must survive an unwritable cache.
        }
    }
    if (-not $script:directConnected) {
        Write-AgentStatus "direct controller connected: $($parts[1]) at $Endpoint"
    }
    $script:directConnected = $true
    $script:directStatusKnown = $true
}

function Read-DirectAcknowledgements {
    while ($udp.Available -gt 0) {
        $remoteEndpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        try {
            $responseBytes = $udp.Receive([ref]$remoteEndpoint)
        }
        catch [Net.Sockets.SocketException] {
            # Routed networks may reject UDP while HTTP to the same controller works.
            break
        }
        $response = [Text.Encoding]::ASCII.GetString($responseBytes)
        if ($remoteEndpoint.Port -ne $DirectPort -or
            ($remoteEndpoint.Address.ToString() -ne $DirectHost -and $DirectHost -ne '255.255.255.255')) { continue }
        if ($response -clike 'LFD[12]_RAM_ACK,*') { continue } # calendar ACKs cannot adopt controller/color
        if ($response -clike 'LFT2_ACK,*') {
            [void](Test-WeeklyTokenAcknowledgement $response)
            continue
        }
        Confirm-DirectController $response "$($remoteEndpoint.Address):$($remoteEndpoint.Port)" $remoteEndpoint.Address.ToString()
    }

    $now = Get-Date
    $referenceTime = if ($script:lastDirectAckAt -eq [DateTime]::MinValue) {
        $script:directStartedAt
    }
    else {
        $script:lastDirectAckAt
    }
    if ($now - $referenceTime -ge [TimeSpan]::FromSeconds(10) -and
        (-not $script:directStatusKnown -or $script:directConnected)) {
        Write-AgentStatus "direct controller unavailable: ${DirectHost}:$DirectPort"
        $script:directConnected = $false
        $script:directStatusKnown = $true
    }
}

function Get-ControllerCandidates([string]$Address) {
    $ip = $null
    if (-not [Net.IPAddress]::TryParse($Address, [ref]$ip) -or
        $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) { return }
    $octets = $ip.GetAddressBytes()
    $private = $octets[0] -eq 10 -or
        ($octets[0] -eq 172 -and $octets[1] -ge 16 -and $octets[1] -le 31) -or
        ($octets[0] -eq 192 -and $octets[1] -eq 168)
    if (-not $private) { return }
    # Search only the known controller's private /24, never the entire LAN.
    $prefix = "$($octets[0]).$($octets[1]).$($octets[2])"
    foreach ($suffix in 1..254) { if ($suffix -ne $octets[3]) { "$prefix.$suffix" } }
}

function Find-ControllerPage([string[]]$Addresses, [int]$HttpPort) {
    $requests = @()
    try {
        foreach ($address in $Addresses) {
            $request = [Net.HttpWebRequest]::Create("http://${address}:$HttpPort/")
            $request.Timeout = 1500
            $request.ReadWriteTimeout = 750
            $request.Proxy = $null
            $request.AllowAutoRedirect = $false
            $request.KeepAlive = $false
            $requests += @{ Host = $address; Request = $request; Pending = $request.BeginGetResponse($null, $null) }
        }
        # Send a real GET on the first connection; never hold an empty MCU socket.
        $deadline = [DateTime]::UtcNow.AddMilliseconds(1500)
        while ([DateTime]::UtcNow -lt $deadline -and
            @($requests | Where-Object { -not $_.Pending.IsCompleted }).Count -gt 0) {
            Start-Sleep -Milliseconds 10
        }
        foreach ($probe in $requests) {
            if (-not $probe.Pending.IsCompleted) { $probe.Request.Abort(); continue }
            try {
                $response = $probe.Request.EndGetResponse($probe.Pending)
                try {
                    $stream = $response.GetResponseStream()
                    $reader = [IO.StreamReader]::new($stream)
                    try {
                        $buffer = [char[]]::new(2048)
                        $count = 0
                        $readDeadline = [DateTime]::UtcNow.AddMilliseconds(750)
                        while ($count -lt $buffer.Length -and [DateTime]::UtcNow -lt $readDeadline) {
                            if ($stream.CanTimeout) {
                                $stream.ReadTimeout = [Math]::Max(1, [int]($readDeadline - [DateTime]::UtcNow).TotalMilliseconds)
                            }
                            $received = $reader.Read($buffer, $count, $buffer.Length - $count)
                            if ($received -eq 0) { break }
                            $count += $received
                            if ([string]::new($buffer, 0, $count) -match '<title>LED Forge Controller</title>') {
                                $probe.Host
                                break
                            }
                        }
                    }
                    finally { $reader.Dispose() }
                }
                finally { $response.Dispose() }
            }
            catch [Net.WebException] { }
            catch [IO.IOException] { }
        }
    }
    finally { foreach ($probe in $requests) { $probe.Request.Abort() } }
}

function Recover-DirectController {
    $now = Get-Date
    if ($null -ne $discoveryUdp) {
        $received = 0
        while ($discoveryUdp.Available -gt 0 -and $received -lt 8) {
            $received++
            $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
            try { $packet = $discoveryUdp.Receive([ref]$remote) }
            catch [Net.Sockets.SocketException] { break }
            if ($directConnected) { continue }
            $hello = [Text.Encoding]::ASCII.GetString($packet)
            if ($hello -cnotmatch '\ALF1_HELLO,([0-9A-F]{6}),-?\d{1,3}\z') { continue }
            if (-not [string]::IsNullOrEmpty($controllerDeviceId) -and $Matches[1] -ne $controllerDeviceId) { continue }
            $candidate = $remote.Address.ToString()
            # HELLO alone never marks the account linked; require the normal ACK.
            $script:DirectHost = $candidate
            $script:nextDirectHttpAt = [DateTime]::MinValue
            Send-Heartbeat
            Read-DirectAcknowledgements
            if ($directConnected) { $script:discoveryCandidates = @(); return }
            $script:DirectHost = $confirmedDirectHost
        }
    }
    if ($directConnected -or $now -lt $nextDiscoveryAt -or
        $now - $directStartedAt -lt [TimeSpan]::FromSeconds(10)) { return }
    if ($discoveryCandidates.Count -eq 0) {
        $script:discoveryCandidates = @(Get-ControllerCandidates $DirectHost)
        # Include current private adapter /24s if the Wi-Fi network itself changed.
        foreach ($adapter in [Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
            if ($adapter.OperationalStatus -ne [Net.NetworkInformation.OperationalStatus]::Up -or
                $adapter.NetworkInterfaceType -eq [Net.NetworkInformation.NetworkInterfaceType]::Loopback) { continue }
            foreach ($address in $adapter.GetIPProperties().UnicastAddresses) {
                if ($address.Address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork) {
                    $script:discoveryCandidates += @(Get-ControllerCandidates $address.Address.ToString())
                }
            }
        }
        $script:discoveryCandidates = @($discoveryCandidates | Select-Object -Unique | Select-Object -First 1024)
        $script:discoveryIndex = 0
        if ($discoveryCandidates.Count -gt 0) {
            Write-AgentStatus "searching controller subnet near $DirectHost"
        }
    }
    if ($discoveryIndex -ge $discoveryCandidates.Count) {
        $script:discoveryCandidates = @()
        $script:nextDiscoveryAt = $now.AddSeconds(30)
        return
    }
    $end = [Math]::Min($discoveryIndex + 7, $discoveryCandidates.Count - 1)
    $candidates = @(Find-ControllerPage $discoveryCandidates[$discoveryIndex..$end] $DirectHttpPort)
    $script:discoveryIndex = $end + 1
    foreach ($candidate in $candidates) {
        Write-AgentStatus "controller candidate found: $candidate; verifying acknowledgement"
        $script:DirectHost = $candidate
        $script:nextDirectHttpAt = [DateTime]::MinValue
        Send-Heartbeat
        Read-DirectAcknowledgements
        if ($directConnected) { $script:discoveryCandidates = @(); break }
        $script:DirectHost = $confirmedDirectHost
    }
}

$mutex = [System.Threading.Mutex]::new($false, "Local\$InstanceName")
$ownsMutex = $false
try {
    try {
        $ownsMutex = $mutex.WaitOne(0)
    }
    catch [System.Threading.AbandonedMutexException] {
        # WaitOne grants ownership when the previous owning thread has exited.
        $ownsMutex = $true
    }
    if (-not $ownsMutex) {
        Write-AgentStatus 'Another agent owns this instance; exiting.'
        return
    }

    $chatEnabled = $EnableChat -and [Environment]::UserInteractive -and [Diagnostics.Process]::GetCurrentProcess().SessionId -gt 0
    $chatRetry = $null
    Write-AgentStatus 'Codex LAN activity agent started.'
    Write-AgentStatus "Client: $clientId"
    Write-AgentStatus "Optional LED Forge preview: ${CollectorHost}:$Port"
    Write-AgentStatus "Direct controller: ${DirectHost}:$DirectPort"
    Write-AgentStatus "Direct controller HTTP: http://${DirectHost}:$DirectHttpPort/agent"
    Write-AgentStatus "Agent name: $($metadata.Name); color: $($metadata.ColorHex6)"
    Write-AgentStatus 'Transmitting client identity, activity, remaining limit, and display metadata.'

    $originHttpSender = @{ Worker=$null; Pending=$null; Stopping=$null; Started=[DateTimeOffset]::MinValue }
    foreach ($provider in @('codex', 'claude')) {
        $resetHttpSenders[$provider] = @{ Worker=$null; Pending=$null; Stopping=$null; Started=[DateTimeOffset]::MinValue }
    }
    $tokenScanner = New-WeeklyTokenScanner
    $claudeDailyScanner = New-WeeklyTokenScanner
    if (-not $DisableLimitPolling) {
        $claudePoller = New-ClaudeQuotaPoller
        $claudeHttpSender = @{ Worker=$null; Pending=$null; Stopping=$null; Started=[DateTimeOffset]::MinValue }
    }

    while ($true) {
        if ($UpdateControlPath) {
            if (Test-Path -LiteralPath "$UpdateControlPath.stop") {
                $managedStop = $true
                Write-AgentStatus 'Worker accepted the supervisor stop request.'
                break
            }
            [IO.File]::WriteAllText("$UpdateControlPath.health", [DateTime]::UtcNow.Ticks.ToString())
        }
        $now = Get-Date
        if ($chatEnabled -and -not $chatSession -and (-not $chatRetry -or $chatRetry.Elapsed.TotalSeconds -ge 30)) {
            try {
                . (Join-Path $PSScriptRoot 'agent_update.ps1')
                . (Join-Path $PSScriptRoot 'agent_chat_ipc.ps1')
                $chatSession = Start-AgentChatSession $PSScriptRoot $ControllerStatePath $clientHash (Join-Path $env:LOCALAPPDATA 'CodexLanAgent/chat') $metadata.Name
            } catch {
                $chatRetry = [Diagnostics.Stopwatch]::StartNew()
                Write-AgentStatus 'Chat startup failed; retrying in 30 seconds.'
            }
        }
        if ($null -ne $claudePoller) { Update-ClaudeQuotaPoller $claudePoller }
        Update-ClaudeHttpSender $claudeHttpSender
        Update-ClaudeHttpSender $originHttpSender
        foreach ($sender in $resetHttpSenders.Values) { Update-ClaudeHttpSender $sender }
        $reportIdentity = ("$AccountFingerprint,$clientHash,$($metadata.ColorHex6),$($metadata.NameUtf8Hex)").ToUpperInvariant()
        try {
            $committedTokens = Receive-WeeklyTokenScan $tokenScanner
            if ($null -ne $committedTokens) {
                $weeklyTokens = $committedTokens
                $weeklyTokensReady = $true
                if ($null -ne $committedTokens.Daily) {
                    $codexDailyTokens = Update-DailyReplay $codexDailyTokens $committedTokens.Daily $committedTokens.ReportIdentity 366 ([DateTimeOffset]$now)
                } else {
                    $codexDailyTokens = $null
                }
                if ($committedTokens.DailyError) { Write-AgentStatus "Daily token checkpoint unavailable; retrying: $($committedTokens.DailyError)" }
            }
            if ($null -eq $tokenScanner.Pending -and $now -ge $nextTokenScan) {
                $nextTokenScan = $now.AddSeconds(15)
                [void](Start-WeeklyTokenScan $tokenScanner (Join-Path $PSScriptRoot 'weekly_tokens.ps1') $SessionsRoot $TokenStatePath -ReportIdentity $reportIdentity)
            }
        }
        catch { Write-AgentStatus "Weekly token checkpoint unavailable; retrying: $($_.Exception.Message)" }
        try {
            $committed = Receive-WeeklyTokenScan $claudeDailyScanner
            if ($null -ne $committed) {
                $claudeDailyTokens = Update-DailyReplay $claudeDailyTokens $committed.Daily $committed.ReportIdentity 28 ([DateTimeOffset]$now)
            }
            if ($null -eq $claudeDailyScanner.Pending -and $now -ge $nextClaudeDailyScan) {
                $nextClaudeDailyScan = $now.AddSeconds(15)
                [void](Start-WeeklyTokenScan $claudeDailyScanner (Join-Path $PSScriptRoot 'weekly_tokens.ps1') $ClaudeRoot "${TokenStatePath}.claude-daily-v1.json" -ReportIdentity $reportIdentity -ClaudeDaily)
            }
        }
        catch {
            $claudeDailyTokens = $null
            Write-AgentStatus "Claude daily token checkpoint unavailable; retrying: $($_.Exception.Message)"
        }
        if (-not $DisableLimitPolling -and $now -ge $nextLimitPoll) {
            $nextLimitPoll = $now.AddSeconds($LimitPollSeconds)
            $previousRemaining = $remainingPercent
            $latestRemaining = Get-RemainingPercent
            if ($null -ne $latestRemaining) {
                Update-AccountRemaining ([double]$latestRemaining)
                if ($remainingPercent -ne $previousRemaining) {
                    Write-AgentStatus "remaining limit: $remainingPercent%"
                }
            }
        }
        foreach ($file in Get-RecentSessionFiles) {
            try {
                Read-NewSessionContent $file
            }
            catch [System.IO.IOException] {
                # Codex may rotate a session while it is being inspected.
            }
        }

        $activity = Get-CurrentActivity
        Send-Heartbeat
        Read-DirectAcknowledgements
        Recover-DirectController
        if ($activity -ne $lastReportedActivity) {
            $state = if ($activity -gt 0) { "active ($activity)" } else { 'idle' }
            Write-AgentStatus $state
            $lastReportedActivity = $activity
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    }
}
finally {
    if ($chatSession) {
        # Only the supervisor's dedicated powershell.exe -File process can finish
        # teardown by exiting the host. Embedded callers retain normal disposal.
        $argv = [Environment]::GetCommandLineArgs()
        $fileArg = [Array]::FindIndex($argv, [Predicate[string]]{ param($arg) $arg -ieq '-File' })
        $dedicatedWorker = $managedStop -and $fileArg -ge 0 -and $fileArg+1 -lt $argv.Length -and
            [IO.Path]::GetFullPath($argv[$fileArg+1]) -ieq $PSCommandPath
        Stop-AgentChatSession $chatSession -WorkerExiting:$dedicatedWorker
        if ($chatSession.TrayStillRunning) {
            Write-AgentStatus 'Chat tray missed shutdown deadline. Finishing worker cleanup before process exit.'
        }
    }
    foreach ($iterator in $claudeDiscoveryStack) { $iterator.Dispose() }
    Stop-LimitAppServer
    if ($null -ne $claudePoller) { Stop-ClaudeQuotaWorker $claudePoller }
    if ($null -ne $claudeHttpSender) { Stop-ClaudeQuotaWorker $claudeHttpSender }
    if ($null -ne $originHttpSender) { Stop-ClaudeQuotaWorker $originHttpSender }
    foreach ($sender in $resetHttpSenders.Values) { Stop-ClaudeQuotaWorker $sender }
    Stop-WeeklyTokenScanner $tokenScanner
    Stop-WeeklyTokenScanner $claudeDailyScanner
    $udp.Dispose()
    if ($null -ne $discoveryUdp) { $discoveryUdp.Dispose() }
    try {
        if ($ownsMutex) { $mutex.ReleaseMutex() }
    }
    finally { $mutex.Dispose() }
    if ($dedicatedWorker -and $chatSession.TrayStillRunning) {
        # Transport and board have joined; token checkpoint writers are now stopped.
        # Normal Windows PowerShell host teardown would synchronously stop the STA again.
        [Environment]::Exit(0)
    }
}
