$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'weekly_tokens.ps1')
function Assert-True($Value, [string]$Message) { if (-not $Value) { throw $Message } }

# Load functions only: never run an installed agent or discovery against the LAN.
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'codex_lan_agent.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
foreach ($name in @('Test-WeeklyTokenAcknowledgement', 'Confirm-DirectController')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    . ([scriptblock]::Create($definition.Extent.Text))
}
$clientHash = 'BA7816BF'; $controllerDeviceId = 'ABC123'
$lastDirectAckAt = [DateTime]::MinValue; $directConnected = $false; $directStatusKnown = $false
$effectiveColorHex6 = '35DDE0'; $DirectHost = '127.0.0.1'; $confirmedDirectHost = $DirectHost
$controllerCacheDirty = $false
$before = @($lastDirectAckAt, $directConnected, $directStatusKnown, $effectiveColorHex6,
    $DirectHost, $confirmedDirectHost, $controllerDeviceId, $controllerCacheDirty) | ConvertTo-Json
Assert-True (Test-WeeklyTokenAcknowledgement 'LFT2_ACK,ABC123,BA7816BF') 'Valid token ACK rejected'
foreach ($ack in @('LFT1_ACK,ABC123,BA7816BF', 'LFT2_ACK,FFFFFF,BA7816BF', 'LFT2_ACK,ABC123,00000000',
    'LFA2_ACK,ABC123,BA7816BF', 'LFT2_ACK,ABC123,BA7816BF,FF4545', 'LFT2_ACK,invalid,BA7816BF')) {
    Assert-True (-not (Test-WeeklyTokenAcknowledgement $ack)) "Foreign/malformed ACK accepted: $ack"
}
Confirm-DirectController 'LFT2_ACK,ABC123,BA7816BF' 'mock' '127.0.0.2'
$after = @($lastDirectAckAt, $directConnected, $directStatusKnown, $effectiveColorHex6,
    $DirectHost, $confirmedDirectHost, $controllerDeviceId, $controllerCacheDirty) | ConvertTo-Json
Assert-True ($before -ceq $after) 'Token ACK changed activity/discovery/color state'

$root = Join-Path ([IO.Path]::GetTempPath()) ('weekly-http-' + [guid]::NewGuid().ToString('N'))
$preview = [Net.Sockets.UdpClient]::new(0)
$direct = [Net.Sockets.UdpClient]::new(0)
$preview.Client.ReceiveTimeout = 100
$server = $null; $process = $null
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    foreach ($name in @('codex_lan_agent.ps1', 'weekly_tokens.ps1', 'agent_metadata.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $root
    }
    $sessions = Join-Path $root 'sessions'
    New-Item -ItemType Directory -Path $sessions | Out-Null
    $event = @{ timestamp = [DateTimeOffset]::UtcNow.AddSeconds(-1).ToString('o'); type = 'event_msg';
        payload = @{ type = 'token_count'; info = @{ total_token_usage = @{ input_tokens = 100; output_tokens = 40 }; last_token_usage = @{ input_tokens = 100; output_tokens = 40 } } } }
    $event | ConvertTo-Json -Depth 6 -Compress |
        Set-Content -LiteralPath (Join-Path $sessions 'test.jsonl') -Encoding UTF8
    $claudeConfig = Join-Path $root 'claude-config'
    $claudeProject = Join-Path $claudeConfig 'projects/sample'
    [void][IO.Directory]::CreateDirectory($claudeProject)
    $claudeAt = [DateTimeOffset]::UtcNow.AddMinutes(-1)
    $claudeLog = Join-Path $claudeProject 'session.jsonl'
    @{ timestamp = $claudeAt.ToString('o'); type = 'assistant'; message = @{ id = 'msg_loopback'
        usage = @{ input_tokens = 7; output_tokens = 4; cache_read_input_tokens = 10 } } } |
        ConvertTo-Json -Depth 6 -Compress | Set-Content -LiteralPath $claudeLog -Encoding UTF8
    [IO.File]::SetLastWriteTimeUtc($claudeLog, [DateTime]::UtcNow.AddDays(-2))
    @{ Name = 'HTTP test'; ColorHex6 = 'ABC123' } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $root 'agent-metadata.json') -Encoding UTF8
    $server = Start-Job -ScriptBlock {
        $ErrorActionPreference = 'Stop'
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $listener.Start()
        "READY:$($listener.LocalEndpoint.Port)"
        $weeklyCount = 0
        try {
            while ($true) {
                $pending = $listener.AcceptTcpClientAsync()
                while (-not $pending.Wait(100)) { }
                $client = $pending.Result
                try {
                    $stream = $client.GetStream(); $stream.ReadTimeout = 2000
                    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII)
                    if ($reader.ReadLine() -notlike 'POST /agent HTTP/*') { throw 'Incorrect HTTP endpoint' }
                    $length = 0
                    while ($header = $reader.ReadLine()) {
                        if ($header -match '^Content-Length: (\d+)$') { $length = [int]$Matches[1] }
                        if ($header -match '^Expect: 100-continue$') {
                            $interim = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 100 Continue`r`n`r`n")
                            $stream.Write($interim, 0, $interim.Length)
                        }
                    }
                    if ($length -le 0 -or $length -ge 200) { throw 'Unbounded HTTP body' }
                    $body = [char[]]::new($length); $read = 0
                    while ($read -lt $length) {
                        $count = $reader.Read($body, $read, $length - $read)
                        if (-not $count) { throw 'Truncated request' }
                        $read += $count
                    }
                    $packet = -join $body; $parts = $packet -split ','
                    [pscustomobject]@{ Packet = $packet; At = [DateTime]::UtcNow }
                    $ack = "LFA2_ACK,ABC123,$($parts[2]),35DDE0"
                    if ($parts[0] -ceq 'LFT2') {
                        $weeklyCount++
                        if ($weeklyCount -eq 1) {
                            # Headers arrive, body stalls: deadline must cover body reads too.
                            $bytes = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: 40`r`nConnection: close`r`n`r`n")
                            $stream.Write($bytes, 0, $bytes.Length)
                            Start-Sleep -Milliseconds 1100
                            continue
                        }
                        $ack = "LFT2_ACK,ABC123,$($parts[2])"
                    }
                    if ($parts[0] -ceq 'LFD2') { $ack = "LFD2_RAM_ACK,ABC123,$($parts[2]),$($parts[3]),$($parts[5])" }
                    $bytes = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: $($ack.Length)`r`nConnection: close`r`n`r`n$ack")
                    $stream.Write($bytes, 0, $bytes.Length)
                }
                finally { $client.Dispose() }
            }
        }
        finally { $listener.Stop() }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(10); $ready = $null
    while (-not $ready -and [DateTime]::UtcNow -lt $deadline) {
        $ready = @(Receive-Job $server -Keep | Where-Object { $_ -is [string] -and $_ -like 'READY:*' }) | Select-Object -First 1
        Start-Sleep -Milliseconds 50
    }
    Assert-True ($null -ne $ready) 'HTTP mock did not start'
    $httpPort = [int]$ready.Substring(6)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $start.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -SessionsRoot "{1}" -CollectorHost 127.0.0.1 -Port {2} -DirectHost 127.0.0.1 -DirectPort {3} -DirectHttpPort {4} -PollMilliseconds 250 -DisableLimitPolling -InstanceName WeeklyHttpTest-{5}' -f
        (Join-Path $root 'codex_lan_agent.ps1'), $sessions, $preview.Client.LocalEndPoint.Port, $direct.Client.LocalEndPoint.Port, $httpPort, $PID
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.EnvironmentVariables['USERPROFILE'] = $root # Keep real Claude sessions outside this idle fixture.
    $start.EnvironmentVariables['CLAUDE_CONFIG_DIR'] = $claudeConfig
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds(34)
    $udpActivity = @(); $udpWeekly = @(); $udpClaude = @(); $lastHeartbeat = $null; $maxGap = 0.0; $previewColorSeen = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) { throw "Test worker exited: $($stderr.Result)" }
        if ($server.State -eq 'Failed') { Receive-Job $server -ErrorAction Stop }
        $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        try {
            $message = [Text.Encoding]::UTF8.GetString($preview.Receive([ref]$remote)) | ConvertFrom-Json
            $now = [DateTime]::UtcNow
            if ($null -ne $lastHeartbeat) { $maxGap = [Math]::Max($maxGap, ($now - $lastHeartbeat).TotalMilliseconds) }
            $lastHeartbeat = $now
            Assert-True ($message.activity -eq 0 -and $message.remaining -eq -1) 'LFA3 activity semantics changed'
            if ($message.color -ceq '35DDE0') { $previewColorSeen = $true }
        }
        catch [Net.Sockets.SocketException] { }
        while ($direct.Available -gt 0) {
            $packet = [Text.Encoding]::ASCII.GetString($direct.Receive([ref]$remote))
            if ($packet -clike 'LFA3,*') { $udpActivity += $packet }
            if ($packet -clike 'LFT2,*') {
                $statePath = Join-Path $root 'weekly-token-state.json'
                Assert-True (Test-Path -LiteralPath $statePath) 'Published before checkpoint commit'
                $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
                Assert-True ($state.Total -eq 140) 'Incomplete committed total'
                $udpWeekly += $packet
            }
            if ($packet -clike 'LFD2,*') {
                $statePath = Join-Path $root 'weekly-token-state.json.claude-daily-v1.json'
                $state = Read-ClaudeDailyTokenState $statePath
                $parts = $packet -split ','
                $day = Get-TokenDay $claudeAt
                Assert-True ($parts[3] -ceq $day -and $state.Days[$day].Revision -eq [long]$parts[5] -and
                    $state.Days[$day].Tokens -eq 21) 'Claude UDP must match durable Kyiv row'
                $udpClaude += $packet
            }
            # Deliberately never send UDP acknowledgements.
        }
    }
    $requests = @(Receive-Job $server -Keep | Where-Object { $_ -isnot [string] })
    $weekly = @($requests | Where-Object { $_.Packet -clike 'LFT2,*' })
    $claude = @($requests | Where-Object { $_.Packet -clike 'LFD2,*' })
    $activity = @($requests | Where-Object { $_.Packet -clike 'LFA3,*' })
    Assert-True ($weekly.Count -eq 3 -and $udpWeekly.Count -eq 3) "Expected three 15-second weekly reports, got HTTP=$($weekly.Count) UDP=$($udpWeekly.Count)"
    foreach ($request in $weekly) {
        Assert-True ($udpWeekly -contains $request.Packet) 'Weekly UDP/HTTP bodies differ'
        $parts = $request.Packet -split ','
        Assert-True ($parts.Count -eq 9 -and $parts[3] -eq (Get-TokenWeek ([DateTimeOffset]::UtcNow)) -and $parts[4] -eq '140' -and $parts[5] -eq '140' -and [long]$parts[6] -gt 0 -and [long]$parts[6] -le 281474976710655) 'Incorrect weekly body'
    }
    $firstRevision = ($weekly[0].Packet -split ',')[6]
    foreach ($request in $weekly) {
        Assert-True ((($request.Packet -split ',')[6]) -eq $firstRevision) 'Unchanged totals caused revision-only traffic'
    }
    for ($i = 1; $i -lt $weekly.Count; $i++) {
        Assert-True (($weekly[$i].At - $weekly[$i - 1].At).TotalSeconds -ge 14.5) 'Weekly retry flooded HTTP'
    }
    Assert-True ($claude.Count -ge 3 -and $udpClaude.Count -ge $claude.Count) 'Claude reports missing or stopped after a RAM ACK'
    foreach ($request in $claude) {
        Assert-True ($request.Packet -ceq $udpClaude[0] -and
            $request.Packet -cmatch '^LFD2,53197668,[0-9A-F]{8},[0-9]{8},21,[0-9]+,ABC123,485454502074657374$') 'Claude wire body or stable revision differs over HTTP'
    }
    Assert-True ($activity.Count -ge 12) 'Activity HTTP cadence regressed'
    foreach ($request in $activity) {
        Assert-True ($udpActivity -contains $request.Packet) 'LFA3 HTTP/UDP bodies differ'
        Assert-True ($request.Packet -cmatch '^LFA3,53197668,[0-9A-F]{8},0,-1,00000000,ABC123,485454502074657374$') 'LFA3 wire format changed'
    }
    Assert-True ($previewColorSeen) 'LFA2 ACK effective color was not applied'
    Assert-True ($maxGap -lt 1800) "Stalled weekly HTTP blocked heartbeat: ${maxGap}ms"
    Assert-True (Test-Path -LiteralPath (Join-Path $root 'controller-endpoint.json')) 'HTTP activity ACK did not persist controller'
    Write-Host "PASS loopback UDP/HTTP: committed Codex weekly=140 and Claude daily=21; bounded stalled body; stable retries; LFA3 heartbeat; max gap=$([int]$maxGap)ms"
}
finally {
    if ($null -ne $process) {
        if (-not $process.HasExited) { $process.Kill() }
        $process.WaitForExit(); $process.Dispose()
    }
    if ($null -ne $server) { Stop-Job $server; Remove-Job $server -Force }
    $preview.Dispose(); $direct.Dispose()
    $resolved = [IO.Path]::GetFullPath($root)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
