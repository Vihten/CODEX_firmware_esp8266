$script:WeeklyTokenMaximum = [long]9007199254740991
$script:WeeklyTokenRecordLimit = 262144
$script:KyivTimeZone = [TimeZoneInfo]::FindSystemTimeZoneById('FLE Standard Time')

function Get-TokenWeek([DateTimeOffset]$Timestamp) {
    $local = [TimeZoneInfo]::ConvertTime($Timestamp, $script:KyivTimeZone)
    $days = (([int]$local.DayOfWeek + 6) % 7)
    return $local.Date.AddDays(-$days).ToString('yyyyMMdd', [Globalization.CultureInfo]::InvariantCulture)
}

function New-WeeklyTokenState([string]$Week) {
    return @{ Version = 2; Week = $Week; Total = [long]0; LifetimeTotal = [long]0; Revision = [long]0; Events = @{}; Files = @{}; Snapshots = @{}; Metadata = @{}; ReportIdentity = ''; PublishedSnapshot = $null }
}

function Read-WeeklyTokenState([string]$Path, [string]$Week) {
    $state = New-WeeklyTokenState $Week
    if (-not (Test-Path -LiteralPath $Path)) { return $state }
    try {
        $saved = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($saved.Version -ne 2) {
            # Preserve exact original bytes and metadata before any V2 commit.
            $backup = "$Path.pre-v2.bak"
            if (Test-Path -LiteralPath $backup) { $backup = "$Path.pre-v2.$([guid]::NewGuid().ToString('N')).bak" }
            [IO.File]::Copy($Path, $backup, $false)
            $state.Metadata = @{ MigratedFromVersion = $saved.Version; Backup = $backup;
                OriginalState = $saved; RebuiltFromLocalLogs = $true }
            return $state
        }
        $total = ConvertTo-TokenInteger $saved.Total
        $lifetime = ConvertTo-TokenInteger $saved.LifetimeTotal
        $revision = ConvertTo-TokenInteger $saved.Revision
        if ($null -eq $total -or $null -eq $lifetime -or $null -eq $revision -or
            $revision -gt 281474976710655) { throw 'Invalid V2 checkpoint' }
        $state.Week = [string]$saved.Week
        $state.Total = $total; $state.LifetimeTotal = $lifetime; $state.Revision = $revision
        $state.Metadata = $saved.Metadata
        $state.PublishedSnapshot = $saved.PublishedSnapshot
        $state.ReportIdentity = [string]$saved.ReportIdentity
        # The embedded ledger is authoritative after the first shared commit.
        # Validate it in the worker before changing or publishing any totals.
        $state.DailyLedger = ConvertTo-DailyTokenMap (Get-DailyTokenField $saved 'DailyLedger')
        foreach ($entry in $saved.Events.PSObject.Properties) { $state.Events[$entry.Name] = $true }
        foreach ($entry in $saved.Snapshots.PSObject.Properties) { $state.Snapshots[$entry.Name] = $entry.Value }
        foreach ($entry in $saved.Files.PSObject.Properties) {
            $f = @{}
            foreach ($property in $entry.Value.PSObject.Properties) { $f[$property.Name] = $property.Value }
            $state.Files[$entry.Name] = $f
        }
        return $state
    }
    catch {
        # A corrupt checkpoint is retained for diagnosis, then rebuilt from raw logs.
        $backup = "$Path.corrupt.$([guid]::NewGuid().ToString('N')).bak"
        [IO.File]::Copy($Path, $backup, $false)
        $state = New-WeeklyTokenState $Week
        $state.Metadata = @{ RecoveryBackup = $backup; RebuiltFromLocalLogs = $true }
        return $state
    }
}

function Save-WeeklyTokenState($State, [string]$Path) {
    $temporary = "$Path.$PID.tmp"
    try {
        $previousRevision = [long]$State.Revision
        $previousPublished = $State.PublishedSnapshot
        $published = $State.PublishedSnapshot
        if ($null -eq $published -or $previousRevision -eq 0 -or $published.Week -ne $State.Week -or
            $published.Total -ne $State.Total -or $published.LifetimeTotal -ne $State.LifetimeTotal -or
            [string]$published.ReportIdentity -cne [string]$State.ReportIdentity) {
            $milliseconds = [long][Math]::Floor(([DateTimeOffset]::UtcNow - [DateTimeOffset]::Parse('1970-01-01T00:00:00Z')).TotalMilliseconds)
            $revision = [Math]::Max($milliseconds, $previousRevision + 1)
            if ($revision -gt 281474976710655) { throw 'Token revision exhausted' }
            $State.Revision = $revision
            $State.PublishedSnapshot = @{ Week = $State.Week; Total = $State.Total; LifetimeTotal = $State.LifetimeTotal; ReportIdentity = [string]$State.ReportIdentity }
        }
        $json = $State | ConvertTo-Json -Compress -Depth 32
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
        $stream = [IO.FileStream]::new($temporary, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
        finally { $stream.Dispose() }
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $Path) }
    }
    catch { $State.Revision = $previousRevision; $State.PublishedSnapshot = $previousPublished; throw }
    finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
}

function ConvertTo-TokenInteger($Value) {
    if ($null -eq $Value -or $Value -is [bool]) { return $null }
    $text = [Convert]::ToString($Value, [Globalization.CultureInfo]::InvariantCulture)
    $number = [long]0
    if ($text -cnotmatch '\A[0-9]+\z' -or -not [long]::TryParse($text, [ref]$number) -or
        $number -gt $script:WeeklyTokenMaximum) { return $null }
    return $number
}

function Add-WeeklyTokenLine($State, $FileState, [string]$Line, [DateTimeOffset]$Now) {
    if ($Line -match '"session_meta"') {
        try {
            $meta = $Line | ConvertFrom-Json
            if ($meta.type -eq 'session_meta') {
                $FileState.SessionId = [string]$meta.payload.id
                $FileState.Parent = [string]$meta.payload.forked_from_id
            }
        } catch { }
        return
    }
    if ($Line -notmatch '"token_count"') { return }
    try { $event = $Line | ConvertFrom-Json } catch { return }
    if ($event.type -ne 'event_msg' -or $event.payload.type -ne 'token_count') { return }
    $usage = $event.payload.info.total_token_usage
    $inputTokens = ConvertTo-TokenInteger $usage.input_tokens
    $outputTokens = ConvertTo-TokenInteger $usage.output_tokens
    if ($null -eq $inputTokens -or $null -eq $outputTokens) { return }
    $timestamp = [DateTimeOffset]::MinValue
    # PS7 ConvertFrom-Json may materialize ISO dates; PS5.1 keeps strings.
    $stamp = $event.timestamp
    if ($stamp -is [DateTime]) { $stamp = $stamp.ToString('o') }
    if ($stamp -is [DateTimeOffset]) { $stamp = $stamp.ToString('o') }
    if ([string]$stamp -cnotmatch '(Z|[+-][0-9]{2}:[0-9]{2})\z' -or
        -not [DateTimeOffset]::TryParse([string]$stamp, [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$timestamp)) { return }
    if ($timestamp -gt $Now) { $FileState.Deferred = $true; return }
    $snapshot = '{0}:{1}' -f $inputTokens, $outputTokens
    $eventKey = '{0}:{1}:{2}:{3}' -f $FileState.Family, $timestamp.UtcDateTime.Ticks, $inputTokens, $outputTokens
    if (-not $FileState.Seen) {
        $last = $event.payload.info.last_token_usage
        $li = ConvertTo-TokenInteger $last.input_tokens
        $lo = ConvertTo-TokenInteger $last.output_tokens
        if ($null -ne $li -and $null -ne $lo -and $li -le $inputTokens -and $lo -le $outputTokens) {
            $baseline = '{0}:{1}' -f ($inputTokens - $li), ($outputTokens - $lo)
            # Only seed inherited initial usage with explicit lineage or an exact
            # cross-file predecessor. Ordinary first records retain their full total.
            $predecessor = $State.Snapshots[$baseline]
            $crossFile = $null -ne $predecessor -and $FileState.Source -and
                $predecessor.Source -ne $FileState.Source -and [long]$predecessor.Ticks -le $timestamp.UtcDateTime.Ticks -and
                (-not $FileState.Family -or -not $predecessor.Family -or $FileState.Family -eq $predecessor.Family)
            if (-not $State.Events.ContainsKey($eventKey) -and ($FileState.Parent -or $crossFile)) {
                $FileState.Input = $inputTokens - $li; $FileState.Output = $outputTokens - $lo
            }
        }
        $FileState.Seen = $true
    }
    # Channels can reset independently. Never replay an unchanged multi-billion
    # input baseline because output alone rolled back (or vice versa).
    if ($inputTokens -lt $FileState.Input) { $FileState.Input = [long]0 }
    if ($outputTokens -lt $FileState.Output) { $FileState.Output = [long]0 }
    $delta = ($inputTokens - $FileState.Input) + ($outputTokens - $FileState.Output)
    $FileState.Input = $inputTokens; $FileState.Output = $outputTokens
    # A status echo can repeat this counter after a fork's first observation.
    # Keep the earliest evidence from this source, including across restarts;
    # otherwise the timestamp guard rejects a real predecessor and replays billions.
    $observed = $State.Snapshots[$snapshot]
    if ($null -eq $observed -or $observed.Source -ne $FileState.Source -or
        [string]$observed.Family -ne [string]$FileState.Family -or
        [long]$observed.Ticks -gt $timestamp.UtcDateTime.Ticks) {
        $State.Snapshots[$snapshot] = @{ Source = $FileState.Source; Ticks = $timestamp.UtcDateTime.Ticks; Family = $FileState.Family }
    }
    if ($delta -le 0) { return }
    # Forks copy timestamped cumulative prefixes. Do not include the file path in this key.
    $key = $eventKey
    if ($State.Events.ContainsKey($key)) { return }
    $State.Events[$key] = $true
    $State.LifetimeTotal = [Math]::Min($script:WeeklyTokenMaximum, $State.LifetimeTotal + $delta)
    if ((Get-TokenWeek $timestamp) -eq $State.Week) {
        $State.Total = [Math]::Min($script:WeeklyTokenMaximum, $State.Total + $delta)
    }
}

function Update-WeeklyTokenState($State, [string]$SessionsRoot, [DateTimeOffset]$Now) {
    $week = Get-TokenWeek $Now
    if ($State.Week -ne $week) {
        $State.Week = $week; $State.Total = [long]0
    }
    $changed = $false
    $files = @(Get-ChildItem -LiteralPath $SessionsRoot -Filter '*.jsonl' -File -Recurse -ErrorAction Stop | Sort-Object Name, FullName)
    # Resolve explicit parents before children regardless of directory enumeration.
    # Read only a bounded header, sharing with the active Codex writer.
    $byId = @{}; $parents = @{}; $firstTimes = @{}
    foreach ($candidate in $files) {
        $headerStream = [IO.File]::Open($candidate.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            $headerReader = [IO.StreamReader]::new($headerStream)
            try {
                $headerBuffer = [char[]]::new(16384)
                $headerCount = $headerReader.Read($headerBuffer, 0, $headerBuffer.Length)
                $header = ([string]::new($headerBuffer, 0, $headerCount) -split "`n", 2)[0]
                try {
                    $meta = $header | ConvertFrom-Json
                    if ($meta.timestamp) { $firstTimes[$candidate.FullName] = ([DateTimeOffset]$meta.timestamp).UtcDateTime.Ticks }
                    if ($meta.type -eq 'session_meta' -and $meta.payload.id) {
                        $byId[[string]$meta.payload.id] = $candidate.FullName
                        $parents[$candidate.FullName] = [string]$meta.payload.forked_from_id
                    }
                } catch { }
            } finally { $headerReader.Dispose() }
        } finally { $headerStream.Dispose() }
    }
    $files = @($files | Sort-Object @{ Expression = { if ($firstTimes.ContainsKey($_.FullName)) { $firstTimes[$_.FullName] } else { [long]0 } } }, Name, FullName)
    $ordered = [Collections.Generic.List[object]]::new()
    $remainingFiles = @{}; foreach ($candidate in $files) { $remainingFiles[$candidate.FullName] = $candidate }
    while ($remainingFiles.Count) {
        $progress = $false
        foreach ($candidate in $files) {
            $candidatePath = $candidate.FullName
            if (-not $remainingFiles.ContainsKey($candidatePath)) { continue }
            $parentId = $parents[$candidatePath]
            if ($parentId -and $byId.ContainsKey($parentId) -and $remainingFiles.ContainsKey($byId[$parentId])) { continue }
            $ordered.Add($candidate); $remainingFiles.Remove($candidatePath); $progress = $true
        }
        if (-not $progress) { throw 'Cyclic token session lineage; checkpoint not committed.' }
    }
    foreach ($file in $ordered) {
        $path = $file.FullName
        if (-not $State.Files.ContainsKey($path) -or $file.Length -lt $State.Files[$path].Offset) {
            $State.Files[$path] = @{ Offset = [long]0; Remainder = ''; Input = [long]0; Output = [long]0; Seen = $false; Parent = ''; SessionId = '' }
            $changed = $true
        }
        $checkpoint = $State.Files[$path]
        $checkpoint.Source = $path
        $familyPath = $path
        $familyId = ''
        foreach ($id in $byId.Keys) { if ($byId[$id] -eq $path) { $familyId = $id; break } }
        while ($parents[$familyPath]) {
            $familyId = $parents[$familyPath]
            if (-not $byId.ContainsKey($familyId)) { break }
            $familyPath = $byId[$familyId]
        }
        $checkpoint.Family = $familyId
        if ($file.Length -eq $checkpoint.Offset) { continue }
        $originalOffset = $checkpoint.Offset
        $originalRemainder = $checkpoint.Remainder
        $originalSkipping = [bool]$checkpoint.Skipping
        $checkpoint.Deferred = $false
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            [void]$stream.Seek($checkpoint.Offset, [IO.SeekOrigin]::Begin)
            $reader = [IO.StreamReader]::new($stream)
            try {
                $buffer = [char[]]::new(32768)
                while (($count = $reader.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $chunk = [string]::new($buffer, 0, $count)
                    if ($checkpoint.Skipping) {
                        $newline = $chunk.IndexOf("`n")
                        if ($newline -lt 0) { continue }
                        $chunk = $chunk.Substring($newline + 1)
                        $checkpoint.Skipping = $false
                    }
                    $lines = ([string]$checkpoint.Remainder + $chunk) -split "`n"
                    $checkpoint.Remainder = $lines[-1]
                    for ($i = 0; $i -lt $lines.Count - 1; $i++) {
                        if ($lines[$i].Length -le $script:WeeklyTokenRecordLimit) {
                            Add-WeeklyTokenLine $State $checkpoint $lines[$i].TrimEnd("`r") $Now
                            if ($checkpoint.Deferred) { break }
                        }
                    }
                    if ($checkpoint.Deferred) { break }
                    if ($checkpoint.Remainder.Length -gt $script:WeeklyTokenRecordLimit) {
                        $checkpoint.Remainder = ''; $checkpoint.Skipping = $true
                    }
                }
                $checkpoint.Offset = $stream.Position
            }
            finally { $reader.Dispose() }
        }
        finally { $stream.Dispose() }
        # Finished historical logs need not end in a newline; incomplete JSON is ignored.
        if (-not $checkpoint.Deferred -and -not $checkpoint.Skipping -and $checkpoint.Remainder) {
            Add-WeeklyTokenLine $State $checkpoint $checkpoint.Remainder $Now
        }
        if ($checkpoint.Deferred) {
            # A scan-time cutoff may be older than a concurrent append. Replay rather
            # than consuming the final snapshot of a session that subsequently goes idle.
            $checkpoint.Offset = $originalOffset
            $checkpoint.Remainder = $originalRemainder
            $checkpoint.Skipping = $originalSkipping
        }
        $changed = $true
    }
    return $changed
}

function New-WeeklyTokenScanner {
    return @{ Worker = [PowerShell]::Create(); Pending = $null }
}

function Start-WeeklyTokenScan($Scanner, [string]$HelperPath, [string]$SessionsRoot, [string]$StatePath,
    [DateTimeOffset]$Now = [DateTimeOffset]::UtcNow, [string]$ReportIdentity = '', [switch]$ClaudeDaily) {
    if ($null -ne $Scanner.Pending) { return $false }
    $Scanner.Worker.Commands.Clear()
    $Scanner.Worker.Streams.Error.Clear()
    [void]$Scanner.Worker.AddScript({
        param($HelperPath, $SessionsRoot, $StatePath, $Now, $ReportIdentity, $ClaudeDaily)
        $ErrorActionPreference = 'Stop'
        . $HelperPath
        if ($ClaudeDaily) {
            $state = Read-ClaudeDailyTokenState $StatePath
            $changed = Update-ClaudeDailyTokenState $state $SessionsRoot $Now $ReportIdentity
            if ($changed -and ($state.Files.Count -gt 0 -or $state.LastPath -or (Test-Path -LiteralPath $StatePath))) {
                Save-DailyTokenState $state $StatePath $script:ClaudeTokenCheckpointBytes
            }
            [pscustomobject]@{ Daily = @($state.Days.Values | Sort-Object Day -Descending); ReportIdentity = $state.ReportIdentity }
            return
        }
        $state = Read-WeeklyTokenState $StatePath (Get-TokenWeek $Now)
        $migrating = $null -eq $state.DailyLedger
        if ($migrating) {
            # Import retained evidence, including logs no longer on disk. Never
            # discard a corrupt sidecar and silently replace historical dates.
            $state.DailyLedger = Read-DailyTokenState "$StatePath.daily-v1.json"
            if (Test-Path -LiteralPath $StatePath) {
                $backup = "$StatePath.pre-dated-weekly.bak"
                if (-not (Test-Path -LiteralPath $backup)) { [IO.File]::Copy($StatePath, $backup, $false) }
            }
        }
        Assert-DailyTokenState $state.DailyLedger
        # Lifetime retains V2 counters/epochs. Only dated weekly attribution is
        # recounted; do not rebuild lifetime or clear dedup/offset checkpoints.
        [void](Update-WeeklyTokenState $state $SessionsRoot $Now)
        Update-DailyTokenState $state.DailyLedger $SessionsRoot $Now $ReportIdentity
        $total = [long]0
        foreach ($row in $state.DailyLedger.Days.Values) {
            # Update-DailyTokenState bounds dates through today. yyyyMMdd sorts
            # chronologically, so the remaining lower bound is this Kyiv Monday.
            if ([string]$row.Day -clt [string]$state.Week) { continue }
            if ($total -gt $script:WeeklyTokenMaximum - [long]$row.Tokens) { throw 'Weekly dated token total overflow; no snapshot published.' }
            $total += [long]$row.Tokens
        }
        $state.Total = $total
        $state.ReportIdentity = $ReportIdentity
        # One atomic checkpoint contains lifetime, weekly and the exact daily
        # rows returned below. No window with independently committed reports.
        Save-WeeklyTokenState $state $StatePath
        $dailyRows = @($state.DailyLedger.Days.Values | Sort-Object Day -Descending)
        # Only fully scanned, durably committed snapshots cross the boundary.
        [pscustomobject]@{ Week = $state.Week; Total = $state.Total; LifetimeTotal = $state.LifetimeTotal; Revision = $state.Revision; ReportIdentity = $state.ReportIdentity; Daily = $dailyRows; DailyError = '' }
    }).AddArgument($HelperPath).AddArgument($SessionsRoot).AddArgument($StatePath).AddArgument($Now).AddArgument($ReportIdentity).AddArgument($ClaudeDaily.IsPresent)
    $Scanner.Pending = $Scanner.Worker.BeginInvoke()
    return $true
}

function Receive-WeeklyTokenScan($Scanner) {
    if ($null -eq $Scanner.Pending -or -not $Scanner.Pending.IsCompleted) { return $null }
    try {
        $result = $Scanner.Worker.EndInvoke($Scanner.Pending)
        # A shared snapshot is valid only after its single durable commit.
        if ($Scanner.Worker.Streams.Error.Count -ne 0 -or $result.Count -ne 1) { throw "Token scan failed before commit (results=$($result.Count), errors=$($Scanner.Worker.Streams.Error.Count))." }
        return $result[0]
    }
    catch {
        # Windows PowerShell can retain failed pipeline state when a worker is
        # reused. Retry in a fresh runspace; keep the durable checkpoint intact.
        $Scanner.Worker.Dispose()
        $Scanner.Worker = [PowerShell]::Create()
        throw
    }
    finally { $Scanner.Pending = $null }
}

function Stop-WeeklyTokenScanner($Scanner) {
    if ($null -eq $Scanner) { return }
    try {
        if ($null -ne $Scanner.Pending) {
            $Scanner.Worker.Stop()
            try { [void]$Scanner.Worker.EndInvoke($Scanner.Pending) } catch { }
        }
    }
    finally { $Scanner.Pending = $null; $Scanner.Worker.Dispose() }
}

function New-WeeklyTokenPacket($State, [string]$Account, [string]$Client, [string]$Color, [string]$NameHex) {
    if ($Account -cnotmatch '\A[0-9A-Fa-f]{8}\z' -or $Client -cnotmatch '\A[0-9A-Fa-f]{8}\z' -or
        $State.Week -cnotmatch '\A[0-9]{8}\z' -or $Color -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or
        $NameHex -cnotmatch '\A(?:[0-9A-Fa-f]{2}){0,48}\z') { throw 'Invalid weekly token packet fields.' }
    $total = ConvertTo-TokenInteger $State.Total
    if ($null -eq $total) { throw 'Invalid weekly token total.' }
    $lifetime = ConvertTo-TokenInteger $State.LifetimeTotal
    $revision = ConvertTo-TokenInteger $State.Revision
    if ($null -eq $lifetime -or $null -eq $revision -or $revision -gt 281474976710655) { throw 'Invalid lifetime/revision' }
    $packet = 'LFT2,{0},{1},{2},{3},{4},{5},{6},{7}' -f $Account.ToUpperInvariant(), $Client.ToUpperInvariant(),
        $State.Week, $total.ToString([Globalization.CultureInfo]::InvariantCulture),
        $lifetime.ToString([Globalization.CultureInfo]::InvariantCulture), $revision.ToString([Globalization.CultureInfo]::InvariantCulture),
        $Color.ToUpperInvariant(), $NameHex.ToUpperInvariant()
    if ([Text.Encoding]::ASCII.GetByteCount($packet) -ge 200) { throw 'Weekly token packet too large.' }
    return $packet
}

# Daily evidence also defines published weekly usage. Lifetime retains V2
# cumulative history; only dated, observed deltas are attributable to a day/week.
function Get-TokenDay([DateTimeOffset]$Timestamp) {
    return [TimeZoneInfo]::ConvertTime($Timestamp, $script:KyivTimeZone).ToString('yyyyMMdd', [Globalization.CultureInfo]::InvariantCulture)
}

function ConvertTo-DailyTokenMap($Value) {
    if ($Value -is [Collections.IDictionary]) {
        $result = @{}; foreach ($key in $Value.Keys) { $result[$key] = ConvertTo-DailyTokenMap $Value[$key] }; return $result
    }
    if ($Value -is [pscustomobject]) {
        $result = @{}; foreach ($p in $Value.PSObject.Properties) { $result[$p.Name] = ConvertTo-DailyTokenMap $p.Value }; return $result
    }
    return $Value
}

function New-DailyTokenState {
    return @{ Version = 1; Files = @{}; Days = @{}; ReportIdentity = ''; Metadata = @{} }
}

function Assert-DailyTokenRows($state) {
    if ((Get-DailyTokenField $state 'Days') -isnot [hashtable] -or
        (Get-DailyTokenField $state 'ReportIdentity') -isnot [string] -or $state.Days.Count -gt 366) { throw 'Invalid daily checkpoint' }
    foreach ($day in $state.Days.Keys) {
        $row = $state.Days[$day]
        if (-not (Test-DailyTokenDate $day) -or (Get-DailyTokenField $row 'Day') -cne $day -or
            $null -eq (ConvertTo-TokenInteger (Get-DailyTokenField $row 'Tokens')) -or
            $null -eq (ConvertTo-TokenInteger (Get-DailyTokenField $row 'Revision')) -or
            [long]$row.Revision -le 0 -or [long]$row.Revision -gt 281474976710655) { throw 'Invalid daily total/revision' }
    }
}

function Assert-DailyTokenState($state) {
    if ((Get-DailyTokenField $state 'Version') -ne 1 -or
        (Get-DailyTokenField $state 'Files') -isnot [hashtable]) { throw 'Invalid daily checkpoint' }
    Assert-DailyTokenRows $state
    foreach ($file in $state.Files.Values) {
        if ((Get-DailyTokenField $file 'Events') -isnot [hashtable] -or
            (Get-DailyTokenField $file 'Signature') -isnot [string] -or
            (Get-DailyTokenField $file 'SessionId') -isnot [string] -or
            (Get-DailyTokenField $file 'Parent') -isnot [string] -or $file.Events.Count -gt 262144) { throw 'Invalid daily evidence' }
        foreach ($entry in $file.Events.Values) {
            if (-not (Test-DailyTokenDate (Get-DailyTokenField $entry 'Day')) -or
                $null -eq (ConvertTo-TokenInteger (Get-DailyTokenField $entry 'Tokens'))) { throw 'Invalid cached daily event' }
        }
    }
}

function Read-DailyTokenState([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return New-DailyTokenState }
    try {
        $state = ConvertTo-DailyTokenMap (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
        Assert-DailyTokenState $state
        return $state
    } catch {
        # Fail closed. Never rebuild empty state over corrupted historical totals.
        # Preserve bytes once, without generating a new backup every polling tick.
        $backup = "$Path.corrupt.bak"
        if (-not (Test-Path -LiteralPath $backup)) { [IO.File]::Copy($Path, $backup, $false) }
        throw 'Daily checkpoint invalid; daily publication suspended. Preserved original and corruption backup.'
    }
}

# Optional JSON fields are not guaranteed by Codex log schema/version. Access
# explicitly so malformed records are safe even under Set-StrictMode Latest.
function Get-DailyTokenField($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    if ($Object -is [Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Test-DailyTokenDate([string]$Day) {
    $date = [DateTime]::MinValue
    return ($Day -cmatch '\A[0-9]{8}\z' -and [DateTime]::TryParseExact($Day, 'yyyyMMdd',
        [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$date))
}

function Read-DailyTokenFile([string]$Path, [DateTimeOffset]$Now, [string]$Oldest) {
    $result = @{ SessionId = ''; Parent = ''; Events = @{}; Deferred = $false }
    $seen = $false; $previousInput = [long]0; $previousOutput = [long]0; $previousTicks = [long]0
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $reader = [IO.StreamReader]::new($stream)
        try {
            # Bounded record storage: skip oversized lines without retaining them.
            $buffer = [char[]]::new(32768); $remainder = ''; $skipping = $false
            $consume = {
                param([string]$Line)
                if ($Line -notmatch '"session_meta"|"token_count"') { return }
                try { $event = $Line | ConvertFrom-Json } catch { return }
                $eventType = Get-DailyTokenField $event 'type'
                $payload = Get-DailyTokenField $event 'payload'
                if ($eventType -eq 'session_meta') {
                    if (-not $seen) { $result.SessionId = [string](Get-DailyTokenField $payload 'id'); $result.Parent = [string](Get-DailyTokenField $payload 'forked_from_id') }
                    return
                }
                if ($eventType -ne 'event_msg' -or (Get-DailyTokenField $payload 'type') -ne 'token_count') { return }
                $info = Get-DailyTokenField $payload 'info'
                $usage = Get-DailyTokenField $info 'total_token_usage'
                $inputCount = ConvertTo-TokenInteger (Get-DailyTokenField $usage 'input_tokens')
                $outputCount = ConvertTo-TokenInteger (Get-DailyTokenField $usage 'output_tokens')
                if ($null -eq $inputCount -or $null -eq $outputCount) { return }
                $stamp = Get-DailyTokenField $event 'timestamp'
                if ($stamp -is [DateTime] -or $stamp -is [DateTimeOffset]) { $stamp = $stamp.ToString('o') }
                $at = [DateTimeOffset]::MinValue
                if ([string]$stamp -cnotmatch '(Z|[+-][0-9]{2}:[0-9]{2})\z' -or
                    -not [DateTimeOffset]::TryParse([string]$stamp, [Globalization.CultureInfo]::InvariantCulture,
                        [Globalization.DateTimeStyles]::None, [ref]$at)) { return }
                if ($at -gt $Now) { $result.Deferred = $true; return }
                $ticks = $at.UtcDateTime.Ticks
                # A backwards timestamp is not a counter reset. Wait for ordered evidence.
                if ($seen -and $ticks -lt $previousTicks) { return }
                if (-not $seen) {
                    # Never put an inherited cumulative baseline onto its first day.
                    # Explicit last usage is the only attributable first observation.
                    $last = Get-DailyTokenField $info 'last_token_usage'
                    $li = ConvertTo-TokenInteger (Get-DailyTokenField $last 'input_tokens')
                    $lo = ConvertTo-TokenInteger (Get-DailyTokenField $last 'output_tokens')
                    $previousInput = $inputCount; $previousOutput = $outputCount
                    if ($null -ne $li -and $null -ne $lo -and $li -le $inputCount -and $lo -le $outputCount) {
                        $previousInput -= $li; $previousOutput -= $lo
                    }
                    $seen = $true
                }
                # Independent channel reset starts a new epoch for that channel only.
                $di = if ($inputCount -ge $previousInput) { $inputCount - $previousInput } else { $inputCount }
                $do = if ($outputCount -ge $previousOutput) { $outputCount - $previousOutput } else { $outputCount }
                $previousInput = $inputCount; $previousOutput = $outputCount; $previousTicks = $ticks
                $day = Get-TokenDay $at
                if ($day -lt $Oldest) { return }
                if ($di -gt $script:WeeklyTokenMaximum - $do) {
                    throw 'Daily token delta overflow; no checkpoint published.'
                }
                $delta = $di + $do
                if ($delta -le 0) { return }
                $key = '{0}:{1}:{2}' -f $ticks, $inputCount, $outputCount
                $result.Events[$key] = @{ Day = $day; Tokens = $delta }
                if ($result.Events.Count -gt 262144) { throw 'Daily evidence limit exceeded; no partial checkpoint published.' }
            }
            while (($count = $reader.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $chunk = [string]::new($buffer, 0, $count)
                if ($skipping) {
                    $newline = $chunk.IndexOf("`n")
                    if ($newline -lt 0) { continue }
                    $chunk = $chunk.Substring($newline + 1); $skipping = $false
                }
                $lines = ($remainder + $chunk) -split "`n"; $remainder = $lines[-1]
                for ($i = 0; $i -lt $lines.Count - 1; ++$i) {
                    if ($lines[$i].Length -le $script:WeeklyTokenRecordLimit) { . $consume $lines[$i].TrimEnd("`r") }
                }
                if ($remainder.Length -gt $script:WeeklyTokenRecordLimit) { $remainder = ''; $skipping = $true }
            }
            if (-not $skipping -and $remainder) { . $consume $remainder }
        } finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
    return $result
}

function Update-DailyTokenState($State, [string]$SessionsRoot, [DateTimeOffset]$Now, [string]$ReportIdentity = '') {
    $local = [TimeZoneInfo]::ConvertTime($Now, $script:KyivTimeZone)
    $oldest = $local.Date.AddDays(-365).ToString('yyyyMMdd', [Globalization.CultureInfo]::InvariantCulture)
    $today = Get-TokenDay $Now
    foreach ($file in @(Get-ChildItem -LiteralPath $SessionsRoot -Filter '*.jsonl' -File -Recurse -ErrorAction Stop)) {
        $path = $file.FullName
        $signature = '{0}:{1}' -f $file.Length, $file.LastWriteTimeUtc.Ticks
        $cached = $State.Files[$path]
        if ($null -ne $cached -and $cached.Signature -ceq $signature) { continue }
        $parsed = Read-DailyTokenFile $path $Now $oldest
        $file.Refresh()
        # A writer may append during the read. Never cache a moving signature.
        if ($parsed.Deferred -or $signature -cne ('{0}:{1}' -f $file.Length, $file.LastWriteTimeUtc.Ticks)) {
            $parsed.Signature = ''
        } else { $parsed.Signature = $signature }
        $State.Files[$path] = $parsed
    }
    # Retain cached evidence from missing/rotated logs, but never beyond 366 dates.
    $byId = @{}
    foreach ($path in $State.Files.Keys) {
        $file = $State.Files[$path]
        if ($file.SessionId) { $byId[$file.SessionId] = $file }
        foreach ($key in @($file.Events.Keys)) {
            if ($file.Events[$key].Day -lt $oldest -or $file.Events[$key].Day -gt $today) { $file.Events.Remove($key) }
        }
    }
    $evidence = @{}
    foreach ($path in $State.Files.Keys) {
        $file = $State.Files[$path]
        $family = if ($file.SessionId) { $file.SessionId } else { $path }
        $ancestor = $file; $visited = @{}
        while ($ancestor.Parent) {
            if ($visited.ContainsKey($ancestor.Parent)) { throw 'Cyclic daily session lineage; no checkpoint published.' }
            $visited[$ancestor.Parent] = $true; $family = $ancestor.Parent
            if (-not $byId.ContainsKey($family)) { break }
            $ancestor = $byId[$family]
        }
        foreach ($key in $file.Events.Keys) {
            $id = "$family`:$key"; $entry = $file.Events[$key]
            # A copied fork prefix may start at this event; prefer the full
            # observed delta over its explicit-last-only first observation.
            if (-not $evidence.ContainsKey($id) -or $entry.Tokens -gt $evidence[$id].Tokens) { $evidence[$id] = $entry }
        }
    }
    $totals = @{}
    foreach ($entry in $evidence.Values) {
        $day = [string]$entry.Day
        if ([long]$totals[$day] -gt $script:WeeklyTokenMaximum - [long]$entry.Tokens) {
            throw 'Daily token total overflow; no checkpoint published.'
        }
        $totals[$day] = [long]$totals[$day] + [long]$entry.Tokens
    }
    [void](Update-DailyTokenRows $State $totals $Now $oldest $today $ReportIdentity)
}

function Update-DailyTokenRows($State, [hashtable]$Totals, [DateTimeOffset]$Now,
    [string]$Oldest, [string]$Today, [string]$ReportIdentity) {
    $changed = $State.ReportIdentity -cne $ReportIdentity
    # Zero tombstones correct dates that have lost all their attributable evidence.
    foreach ($day in @($State.Days.Keys)) {
        if ($day -lt $Oldest -or $day -gt $Today) {
            $State.Days.Remove($day)
            $changed = $true
            continue
        }
        if (-not $totals.ContainsKey($day)) { $totals[$day] = [long]0 }
    }
    foreach ($day in $totals.Keys) {
        $old = $State.Days[$day]
        if ($null -eq $old -or [long]$old.Tokens -ne [long]$totals[$day] -or $State.ReportIdentity -cne $ReportIdentity) {
            $milliseconds = [long][Math]::Floor(($Now.ToUniversalTime() - [DateTimeOffset]::Parse('1970-01-01T00:00:00Z')).TotalMilliseconds)
            $revision = [Math]::Max($milliseconds, [long](Get-DailyTokenField $old 'Revision') + 1)
            if ($revision -gt 281474976710655) { throw 'Daily token revision exhausted' }
            $State.Days[$day] = @{ Day = $day; Tokens = [long]$totals[$day]; Revision = [long]$revision }
            $changed = $true
        }
    }
    $State.ReportIdentity = $ReportIdentity
    if ($State.Days.Count -gt 366) { throw 'Daily date window exceeded' }
    return $changed
}

function Save-DailyTokenState($State, [string]$Path, [long]$MaximumBytes = [long]::MaxValue) {
    $temporary = "$Path.$PID.tmp"
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($State | ConvertTo-Json -Compress -Depth 32))
        if ($bytes.LongLength -gt $MaximumBytes) { throw 'Daily token checkpoint size limit exceeded' }
        $stream = [IO.FileStream]::new($temporary, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $Path) }
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
}

function New-DailyTokenPacket($Day, [string]$Account, [string]$Client, [string]$Color, [string]$NameHex,
    [ValidateSet('LFD1', 'LFD2')][string]$Protocol = 'LFD1') {
    $date = [DateTime]::MinValue
    if ($Account -cnotmatch '\A[0-9A-Fa-f]{8}\z' -or $Client -cnotmatch '\A[0-9A-Fa-f]{8}\z' -or
        $Client -ceq '00000000' -or
        $Color -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or $NameHex -cnotmatch '\A(?:[0-9A-Fa-f]{2}){0,48}\z' -or
        -not [DateTime]::TryParseExact([string]$Day.Day, 'yyyyMMdd', [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$date)) { throw 'Invalid daily token packet fields' }
    $tokens = ConvertTo-TokenInteger $Day.Tokens; $revision = ConvertTo-TokenInteger $Day.Revision
    if ($null -eq $tokens -or $null -eq $revision -or $revision -le 0 -or $revision -gt 281474976710655) { throw 'Invalid daily token total/revision' }
    $packet = '{0},{1},{2},{3},{4},{5},{6},{7}' -f $Protocol, $Account.ToUpperInvariant(), $Client.ToUpperInvariant(),
        $Day.Day, $tokens.ToString([Globalization.CultureInfo]::InvariantCulture),
        $revision.ToString([Globalization.CultureInfo]::InvariantCulture), $Color.ToUpperInvariant(), $NameHex.ToUpperInvariant()
    if ([Text.Encoding]::ASCII.GetByteCount($packet) -ge 200) { throw 'Daily packet too large' }
    return $packet
}

$script:ClaudeTokenFileLimit = 8192
$script:ClaudeTokenEvidenceLimit = 65536
$script:ClaudeTokenCheckpointBytes = 32MB
$script:ClaudeTokenSliceBytes = 1MB
$script:ClaudeTokenScanBytes = 8MB
$script:ClaudeTokenScanFiles = 128

function Test-ClaudeTokenInteger($Value, [long]$Maximum = [long]::MaxValue) {
    return (($Value -is [int] -or $Value -is [long]) -and $Value -ge 0 -and $Value -le $Maximum)
}

function Read-ClaudeDailyTokenState([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return @{ Version = 1; Files = @{}; Evidence = @{}; Days = @{}; ReportIdentity = ''; LastPath = '' }
    }
    try {
        if ([IO.FileInfo]::new($Path).Length -gt $script:ClaudeTokenCheckpointBytes) { throw 'Checkpoint size limit' }
        $state = ConvertTo-DailyTokenMap (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
        Assert-DailyTokenRows $state
        if ($state.Version -ne 1 -or $state.Files -isnot [hashtable] -or $state.Evidence -isnot [hashtable] -or
            $state.LastPath -isnot [string] -or $state.Files.Count -gt $script:ClaudeTokenFileLimit -or
            $state.Evidence.Count -gt $script:ClaudeTokenEvidenceLimit) { throw 'Invalid Claude token checkpoint' }
        foreach ($cursor in $state.Files.Values) {
            if ($cursor -isnot [hashtable] -or -not (Test-ClaudeTokenInteger $cursor.Offset) -or
                -not (Test-ClaudeTokenInteger $cursor.Length) -or $cursor.Length -lt $cursor.Offset -or
                -not (Test-ClaudeTokenInteger $cursor.Created ([DateTime]::MaxValue.Ticks)) -or
                -not (Test-ClaudeTokenInteger $cursor.Written ([DateTime]::MaxValue.Ticks)) -or
                $cursor.Complete -isnot [bool] -or $cursor.Discard -isnot [bool] -or
                $cursor.Anchor -isnot [string] -or $cursor.Anchor -cnotmatch '\A(?:[A-Za-z0-9+/]{43}=)?\z' -or
                (($cursor.Offset -eq 0) -ne ($cursor.Anchor.Length -eq 0))) { throw 'Invalid Claude token cursor' }
        }
        foreach ($key in $state.Evidence.Keys) {
            $entry = $state.Evidence[$key]
            if ($key -cnotmatch '\A[a-f0-9]{64}\z' -or $entry -isnot [hashtable] -or
                -not (Test-ClaudeTokenInteger $entry.At ([DateTime]::MaxValue.Ticks)) -or
                -not (Test-ClaudeTokenInteger $entry.Tokens $script:WeeklyTokenMaximum)) { throw 'Invalid Claude token evidence' }
        }
        return $state
    }
    catch {
        # Preserve revisions and retained observations if the checkpoint is unreadable.
        throw 'Claude daily checkpoint invalid; preserved original. Publication suspended.'
    }
}

function Find-ClaudeTokenFiles([string]$Root) {
    $files = @{}
    if ([string]::IsNullOrWhiteSpace($Root)) { return $files }
    $rootPath = [IO.Path]::GetFullPath($Root)
    try { $attributes = [IO.File]::GetAttributes($rootPath) }
    catch [IO.FileNotFoundException] { return $files }
    catch [IO.DirectoryNotFoundException] { return $files }
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -or
        -not ($attributes -band [IO.FileAttributes]::Directory)) { return $files }
    $stack = [Collections.Generic.List[object]]::new()
    $stack.Add([IO.Directory]::EnumerateFileSystemEntries($rootPath).GetEnumerator())
    try {
        $visited = 0
        while ($stack.Count -gt 0) {
            $iterator = $stack[$stack.Count - 1]
            if (-not $iterator.MoveNext()) {
                $iterator.Dispose()
                $stack.RemoveAt($stack.Count - 1)
                continue
            }
            if (++$visited -gt 65536) { throw 'Claude transcript discovery limit exceeded' }
            $path = $iterator.Current
            try { $attributes = [IO.File]::GetAttributes($path) }
            catch [IO.FileNotFoundException] { continue }
            catch [IO.DirectoryNotFoundException] { continue }
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($attributes -band [IO.FileAttributes]::Directory) {
                if ($stack.Count -lt 8) { $stack.Add([IO.Directory]::EnumerateFileSystemEntries($path).GetEnumerator()) }
                continue
            }
            if ([IO.Path]::GetExtension($path) -ne '.jsonl') { continue }
            if ($files.Count -ge $script:ClaudeTokenFileLimit) { throw 'Claude transcript file limit exceeded' }
            $files[$path] = [IO.FileInfo]::new($path)
        }
    }
    finally { foreach ($iterator in $stack) { $iterator.Dispose() } }
    return $files
}

function Get-ClaudeTokenAnchor($Stream, [long]$Offset, $Hasher) {
    if ($Offset -eq 0) { return '' }
    $size = [int][Math]::Min(4096L, $Offset)
    $sample = [byte[]]::new(2 * $size)
    $Stream.Position = 0
    if ($Stream.Read($sample, 0, $size) -ne $size) { throw [IO.IOException]::new('Transcript changed during read') }
    $Stream.Position = $Offset - $size
    if ($Stream.Read($sample, $size, $size) -ne $size) { throw [IO.IOException]::new('Transcript changed during read') }
    return [Convert]::ToBase64String($Hasher.ComputeHash($sample))
}

function ConvertFrom-ClaudeTokenLine([string]$Line, [DateTimeOffset]$Now, [string]$Oldest, $Hasher) {
    if ($Line -cnotmatch '"assistant"' -or $Line -cnotmatch '"usage"\s*:') { return }
    try { $record = $Line | ConvertFrom-Json -ErrorAction Stop } catch { return }
    if ((Get-DailyTokenField $record 'type') -cne 'assistant') { return }
    $message = Get-DailyTokenField $record 'message'
    $id = Get-DailyTokenField $message 'id'
    if ($id -isnot [string] -or $id -cnotmatch '\A[A-Za-z0-9_-]{1,160}\z' -or
        (Get-DailyTokenField $message 'model') -ceq '<synthetic>') { return }
    $usage = Get-DailyTokenField $message 'usage'
    $tokens = [long]0
    foreach ($name in @('input_tokens', 'output_tokens', 'cache_creation_input_tokens', 'cache_read_input_tokens')) {
        $raw = Get-DailyTokenField $usage $name
        if ($null -eq $raw -and $name.StartsWith('cache_')) { continue }
        if (-not (Test-ClaudeTokenInteger $raw $script:WeeklyTokenMaximum)) { return }
        $count = [long]$raw
        if ($tokens -gt $script:WeeklyTokenMaximum - $count) { throw 'Claude message token total overflow' }
        $tokens += $count
    }
    $stamp = Get-DailyTokenField $record 'timestamp'
    $at = [DateTimeOffset]::MinValue
    if ($stamp -is [DateTimeOffset]) { $at = $stamp }
    elseif ($stamp -is [DateTime] -and $stamp.Kind -ne [DateTimeKind]::Unspecified) { $at = [DateTimeOffset]$stamp }
    elseif ($stamp -isnot [string] -or $stamp -cnotmatch '(Z|[+-][0-9]{2}:[0-9]{2})\z' -or
        -not [DateTimeOffset]::TryParse($stamp, [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::None, [ref]$at)) { return }
    if ($at -gt $Now.AddDays(1) -or (Get-TokenDay $at) -lt $Oldest) { return }
    $key = [BitConverter]::ToString($Hasher.ComputeHash([Text.Encoding]::ASCII.GetBytes($id))).Replace('-', '').ToLowerInvariant()
    return @{ Key = $key; At = $at.UtcTicks; Tokens = $tokens }
}

function Read-ClaudeTokenSlice([IO.FileInfo]$File, $Cursor, [byte[]]$Buffer, [int]$Limit, $Hasher) {
    # Fix the read window before opening the file. Retry a visibly moving file
    # before accepting any evidence out of the buffer.
    $length = $File.Length
    $created = $File.CreationTimeUtc.Ticks
    $written = $File.LastWriteTimeUtc.Ticks
    $offset = 0L
    $discard = $false
    if ($File.Attributes -band [IO.FileAttributes]::ReparsePoint) { return }
    $stream = [IO.File]::Open($File.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        if ($null -ne $Cursor -and $created -eq $Cursor.Created -and $length -ge $Cursor.Length -and
            ($length -gt $Cursor.Length -or $written -eq $Cursor.Written) -and
            (Get-ClaudeTokenAnchor $stream $Cursor.Offset $Hasher) -ceq $Cursor.Anchor) {
            $offset = [long]$Cursor.Offset
            $discard = $Cursor.Discard
        }
        $stream.Position = $offset
        $count = 0
        $target = [int][Math]::Min([long]$Limit, $length - $offset)
        while ($count -lt $target) {
            $read = $stream.Read($Buffer, $count, $target - $count)
            if ($read -eq 0) { throw [IO.IOException]::new('Transcript shortened during read') }
            $count += $read
        }
        $lastLf = if ($count -gt 0) { [Array]::LastIndexOf($Buffer, [byte]10, $count - 1, $count) } else { -1 }
        $consumed = $lastLf + 1
        $nextDiscard = $discard -and $lastLf -lt 0
        if ($nextDiscard -or $count - $consumed -gt $script:WeeklyTokenRecordLimit) {
            $consumed = $count
            $nextDiscard = $true
        }
        $nextOffset = $offset + $consumed
        $anchor = Get-ClaudeTokenAnchor $stream $nextOffset $Hasher
        $File.Refresh()
        if (-not $File.Exists -or $File.CreationTimeUtc.Ticks -ne $created -or $File.Length -ne $length -or
            $File.LastWriteTimeUtc.Ticks -ne $written -or
            ($File.Attributes -band [IO.FileAttributes]::ReparsePoint)) { return }
        return @{ Count = $count; LastLf = $lastLf; DiscardFirst = $discard; FirstSlice = $offset -eq 0;
            Cursor = @{ Offset = $nextOffset; Length = $length; Created = $created; Written = $written;
                Complete = ($offset + $count -eq $length); Anchor = $anchor; Discard = $nextDiscard } }
    }
    finally { $stream.Dispose() }
}

function Update-ClaudeDailyTokenState($State, [string]$Root, [DateTimeOffset]$Now, [string]$ReportIdentity) {
    $local = [TimeZoneInfo]::ConvertTime($Now, $script:KyivTimeZone)
    $oldest = $local.Date.AddDays(-365).ToString('yyyyMMdd', [Globalization.CultureInfo]::InvariantCulture)
    $today = Get-TokenDay $Now
    $oldestTicks = [TimeZoneInfo]::ConvertTimeToUtc($local.Date.AddDays(-365), $script:KyivTimeZone).Ticks
    $changed = $false
    $lastPath = $State.LastPath
    foreach ($key in @($State.Evidence.Keys)) {
        if ($State.Evidence[$key].At -lt $oldestTicks) {
            $State.Evidence.Remove($key)
            $changed = $true
        }
    }
    $files = Find-ClaudeTokenFiles $Root
    foreach ($path in @($State.Files.Keys)) {
        if (-not $files.ContainsKey($path)) {
            $State.Files.Remove($path)
            $changed = $true
        }
    }
    $paths = [string[]]@($files.Keys)
    [Array]::Sort($paths, [StringComparer]::OrdinalIgnoreCase)
    $start = [Array]::BinarySearch($paths, [string]$State.LastPath, [StringComparer]::OrdinalIgnoreCase)
    $start = if ($start -ge 0) { $start + 1 } else { -bnot $start }
    $buffer = [byte[]]::new($script:ClaudeTokenSliceBytes)
    $decoder = [Text.UTF8Encoding]::new($false, $true)
    $hasher = [Security.Cryptography.SHA256]::Create()
    $remaining = $script:ClaudeTokenScanBytes
    $attempts = 0
    try {
        # Leave enough room for a whole bounded record, otherwise an incomplete
        # record at the budget boundary could repeatedly make no progress.
        for ($i = 0; $i -lt $paths.Count -and $remaining -ge $buffer.Length -and $attempts -lt $script:ClaudeTokenScanFiles; ++$i) {
            $path = $paths[($start + $i) % $paths.Count]
            $file = $files[$path]
            $cursor = $State.Files[$path]
            if (-not $file.Exists -or ($null -ne $cursor -and $cursor.Complete -and $cursor.Length -eq $file.Length -and
                $cursor.Created -eq $file.CreationTimeUtc.Ticks -and $cursor.Written -eq $file.LastWriteTimeUtc.Ticks)) { continue }
            $State.LastPath = $path
            ++$attempts
            try { $slice = Read-ClaudeTokenSlice $file $cursor $buffer $buffer.Length $hasher }
            catch [IO.IOException] { $slice = $null }
            # Charge a failed read at its maximum size as it may have consumed a
            # whole slice before detecting rotation.
            if ($null -eq $slice) { $remaining -= $buffer.Length; continue }
            $remaining -= $slice.Count
            $position = 0
            while ($position -le $slice.LastLf) {
                $newline = [Array]::IndexOf($buffer, [byte]10, $position, $slice.LastLf - $position + 1)
                $lineStart = $position
                $position = $newline + 1
                if (($lineStart -eq 0 -and $slice.DiscardFirst) -or $newline - $lineStart -gt $script:WeeklyTokenRecordLimit) { continue }
                try { $line = $decoder.GetString($buffer, $lineStart, $newline - $lineStart) }
                catch [Text.DecoderFallbackException] { continue }
                if ($slice.FirstSlice -and $lineStart -eq 0) { $line = $line.TrimStart([char]0xFEFF) }
                $entry = ConvertFrom-ClaudeTokenLine $line $Now $oldest $hasher
                if ($null -eq $entry) { continue }
                $prior = $State.Evidence[$entry.Key]
                if ($null -eq $prior) {
                    if ($State.Evidence.Count -ge $script:ClaudeTokenEvidenceLimit) { throw 'Claude token evidence limit exceeded' }
                    $State.Evidence[$entry.Key] = @{ At = $entry.At; Tokens = $entry.Tokens }
                } else {
                    # Content blocks, retries and copied fork prefixes can repeat
                    # a response. Keep one largest snapshot on its first date.
                    $prior.At = [Math]::Min([long]$prior.At, [long]$entry.At)
                    $prior.Tokens = [Math]::Max([long]$prior.Tokens, [long]$entry.Tokens)
                }
            }
            $State.Files[$path] = $slice.Cursor
            $changed = $true
        }
    }
    finally { $hasher.Dispose() }
    $totals = @{}
    foreach ($entry in $State.Evidence.Values) {
        if ($entry.At -gt $Now.UtcTicks -or $entry.Tokens -eq 0) { continue }
        $day = Get-TokenDay ([DateTimeOffset]::new([long]$entry.At, [TimeSpan]::Zero))
        if ([long]$totals[$day] -gt $script:WeeklyTokenMaximum - [long]$entry.Tokens) { throw 'Claude daily token total overflow' }
        $totals[$day] = [long]$totals[$day] + [long]$entry.Tokens
    }
    $rowsChanged = Update-DailyTokenRows $State $totals $Now $oldest $today $ReportIdentity
    return ($changed -or $rowsChanged -or $State.LastPath -cne $lastPath)
}
