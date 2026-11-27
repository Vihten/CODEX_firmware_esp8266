# Remote packages can replace only this fixed code set. Keys and state never enter it.
function Get-AgentCodeFiles {
    @('codex_lan_agent.ps1', 'start_agent_supervisor.ps1', 'agent_metadata.ps1',
      'weekly_tokens.ps1', 'agent_chat_ipc.ps1', 'agent_chat_ui.ps1',
      'agent_update.ps1', 'agent_update_download.ps1', 'agent_update_restart.ps1', 'agent-version.txt')
}

function Test-AgentUpdateReady([string]$Root) {
    try { $hub = Get-AgentHubBinding $Root } catch { return $false }
    $endpointFile = if ($hub) { 'hub-management.json' } else { 'controller-endpoint.json' }
    foreach ($name in @('agent_update.ps1', 'agent_update_download.ps1', 'agent_update_restart.ps1', 'agent-update-public.xml', 'agent-version.txt', $endpointFile)) {
        $path = Join-Path $Root $name
        Assert-AgentPlainPath $path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    }
    $true
}

function Get-AgentSupervisorArguments($Parameters, [string]$Root) {
    $expected = @('CollectorHost', 'Port', 'DirectHost', 'DirectPort', 'DirectHttpPort', 'LogPath', 'InstanceName')
    if (@($Parameters.PSObject.Properties.Name | Where-Object { $_ -notin $expected }).Count -or $Parameters.PSObject.Properties.Count -eq 0) { throw 'Invalid supervisor parameters' }
    foreach ($name in @('CollectorHost', 'DirectHost', 'InstanceName')) {
        if ([string]$Parameters.$name -cnotmatch '\A[a-zA-Z0-9._:-]+\z') { throw 'Invalid supervisor identity/host' }
    }
    foreach ($name in @('Port', 'DirectPort', 'DirectHttpPort')) {
        if ([string]$Parameters.$name -cnotmatch '\A[0-9]+\z' -or $Parameters.$name -lt 1 -or $Parameters.$name -gt 65535) { throw 'Invalid supervisor port' }
    }
    if ([string]$Parameters.InstanceName -cnotmatch '\A[a-zA-Z0-9_-]+\z') { throw 'Invalid supervisor mutex name' }
    $logPath = [IO.Path]::GetFullPath([string]$Parameters.LogPath)
    $supervisor = Join-Path $Root 'start_agent_supervisor.ps1'
    foreach ($value in @($logPath, $supervisor)) {
        if ($value.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0 -or $value.EndsWith('\') -or $value.Contains('"')) { throw 'Invalid launch path' }
    }
    # -File receives literal arguments; no interpolated PowerShell command is evaluated.
    "-NoProfile -ExecutionPolicy Bypass -File `"$supervisor`" -CollectorHost $($Parameters.CollectorHost) -Port $($Parameters.Port) -DirectHost $($Parameters.DirectHost) -DirectPort $($Parameters.DirectPort) -DirectHttpPort $($Parameters.DirectHttpPort) -LogPath `"$logPath`" -InstanceName $($Parameters.InstanceName)"
}

function Assert-AgentPlainPath([string]$Path) {
    $item = [IO.Path]::GetFullPath($Path)
    while ($item) {
        if (Test-Path -LiteralPath $item) {
            if ((Get-Item -LiteralPath $item -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse path rejected' }
        }
        $parent = [IO.Path]::GetDirectoryName($item)
        if ($parent -eq $item) { break }
        $item = $parent
    }
}

function Write-AgentDurable([string]$Path, [byte[]]$Bytes) {
    Assert-AgentPlainPath $Path
    $temp = "$Path.new"
    Assert-AgentPlainPath $temp
    $stream = [IO.FileStream]::new($temp, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None, 4096, [IO.FileOptions]::WriteThrough)
    try { $stream.Write($Bytes, 0, $Bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temp, $Path, [System.Management.Automation.Language.NullString]::Value) }
    else { [IO.File]::Move($temp, $Path) }
}

function ConvertTo-AgentVersion($Value) {
    if ($null -eq $Value -or $Value -is [string] -or $Value -is [bool] -or
        [string]$Value -cnotmatch '\A[0-9]+\z') { throw 'Invalid integer' }
    $number = [decimal]$Value
    if ($number -lt 1 -or $number -gt 9007199254740991) { throw 'Unsafe version' }
    [long]$number
}

function Get-AgentInstalledVersion([string]$Root) {
    Assert-AgentPlainPath (Join-Path $Root 'agent-version.txt')
    $raw = [IO.File]::ReadAllText((Join-Path $Root 'agent-version.txt')).Trim()
    if ($raw -cnotmatch '\A[0-9]+\z') { throw 'Invalid installed version' }
    ConvertTo-AgentVersion ([decimal]$raw)
}

function Test-AgentPackage([string]$Root, [string]$Stage, [long]$InstalledVersion) {
    foreach ($path in @($Root, $Stage, (Join-Path $Root 'agent-update-public.xml'), (Join-Path $Stage 'manifest.json'), (Join-Path $Stage 'package.zip'))) { Assert-AgentPlainPath $path }
    $manifestPath = Join-Path $Stage 'manifest.json'
    if ((Get-Item -LiteralPath $manifestPath).Length -gt 8192) { throw 'Manifest too large' }
    $m = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    if ($m.schema -is [string] -or $m.schema -is [bool] -or $m.schema -ne 1 -or (($m.PSObject.Properties.Name | Sort-Object) -join ',') -cne 'schema,sha256,signature,size,version') { throw 'Manifest schema' }
    $version = ConvertTo-AgentVersion $m.version
    if ($version -le $InstalledVersion) { throw 'Downgrade or replay' }
    if ($m.size -is [string] -or $m.size -is [bool] -or [string]$m.size -cnotmatch '\A[0-9]+\z' -or $m.size -lt 1 -or $m.size -gt 524288) { throw 'Package size bound' }
    if ($m.sha256 -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Invalid digest' }
    $rsa = [Security.Cryptography.RSACryptoServiceProvider]::new()
    $rsa.PersistKeyInCsp = $false
    try {
        $rsa.FromXmlString([IO.File]::ReadAllText((Join-Path $Root 'agent-update-public.xml')))
        if ($rsa.KeySize -lt 2048 -or -not $rsa.PublicOnly) { throw 'Invalid public key' }
        $signed = [Text.Encoding]::UTF8.GetBytes("LFAU1`n$version`n$($m.size)`n$($m.sha256)`n")
        if (-not $rsa.VerifyData($signed, 'SHA256', [Convert]::FromBase64String($m.signature))) { throw 'Invalid signature' }
    } finally { $rsa.Dispose() }
    $zipPath = Join-Path $Stage 'package.zip'
    if ((Get-Item -LiteralPath $zipPath).Length -ne $m.size -or (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $m.sha256) { throw 'Package digest/size mismatch' }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $seen = @{}
        $files = @{}
        $total = 0L
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            if ($name -notin (Get-AgentCodeFiles) -or $seen.ContainsKey($name) -or
                $name -match '[/\\:]' -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -notin @(0, 0x8000) -or
                ($entry.ExternalAttributes -band 0x410) -ne 0) { throw 'ZIP entry rejected' }
            $seen[$name] = $true
            $total += $entry.Length
            if ($entry.Length -lt 1 -or $total -gt 2097152) { throw 'Expanded bound' }
            $inputStream = $entry.Open()
            $memory = [IO.MemoryStream]::new()
            try {
                $buffer = [byte[]]::new(8192)
                while (($count = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    if ($memory.Length + $count -gt $entry.Length) { throw 'ZIP length mismatch' }
                    $memory.Write($buffer, 0, $count)
                }
                if ($memory.Length -ne $entry.Length) { throw 'Truncated ZIP' }
                $files[$name.ToLowerInvariant()] = $memory.ToArray()
            } finally { $inputStream.Dispose(); $memory.Dispose() }
        }
        if ($seen.Count -ne (Get-AgentCodeFiles).Count) { throw 'Missing ZIP entry' }
        $packagedVersion = [Text.Encoding]::UTF8.GetString($files['agent-version.txt']).Trim()
        if ($packagedVersion -cne [string]$version) { throw 'Package version mismatch' }
        [pscustomobject]@{ Version = $version; Files = $files }
    } finally { $zip.Dispose() }
}

function Restore-AgentUpdate([string]$Root) {
    $journal = Join-Path $Root '.agent-update-journal.json'
    Assert-AgentPlainPath $journal
    if (-not (Test-Path -LiteralPath $journal)) { return }
    # Journal contents never choose paths: recovery always restores the complete fixed set.
    foreach ($name in Get-AgentCodeFiles) {
        $backup = Join-Path $Root ".agent-update-backup/$name"
        Assert-AgentPlainPath $backup
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) { throw "Missing recovery backup: $name" }
    }
    foreach ($name in Get-AgentCodeFiles) {
        Write-AgentDurable (Join-Path $Root $name) ([IO.File]::ReadAllBytes((Join-Path $Root ".agent-update-backup/$name")))
    }
    [IO.File]::Delete($journal)
}

function Install-AgentStagedUpdate([string]$Root, [string]$Stage) {
    if (Test-Path -LiteralPath (Join-Path $Root '.agent-update-journal.json')) { throw 'Recovery required' }
    $package = Test-AgentPackage $Root $Stage (Get-AgentInstalledVersion $Root)
    $backup = Join-Path $Root '.agent-update-backup'
    Assert-AgentPlainPath $backup
    [void][IO.Directory]::CreateDirectory($backup)
    foreach ($name in Get-AgentCodeFiles) {
        Assert-AgentPlainPath (Join-Path $Root $name)
        Write-AgentDurable (Join-Path $backup $name) ([IO.File]::ReadAllBytes((Join-Path $Root $name)))
    }
    Write-AgentDurable (Join-Path $Root '.agent-update-journal.json') ([Text.Encoding]::UTF8.GetBytes('{"pending":true}'))
    try {
        foreach ($name in Get-AgentCodeFiles) { Write-AgentDurable (Join-Path $Root $name) $package.Files[$name] }
    } catch { Restore-AgentUpdate $Root; throw }
}

function Complete-AgentUpdate([string]$Root) {
    $path = Join-Path $Root '.agent-update-journal.json'
    Assert-AgentPlainPath $path
    [IO.File]::Delete($path)
}

function Invoke-AgentUpdateRequest([uri]$Uri, [string]$Body, [int]$Maximum) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $request = [Net.HttpWebRequest]::Create($Uri)
    $request.Proxy = $null
    $request.AllowAutoRedirect = $false
    $request.Timeout = 4000
    $request.ReadWriteTimeout = 4000
    $request.KeepAlive = $false
    if ($Body) {
        $request.Method = 'POST'
        $request.ContentType = 'application/x-www-form-urlencoded'
        $bytes = [Text.Encoding]::ASCII.GetBytes($Body)
        $request.ContentLength = $bytes.Length
        $stream = $request.GetRequestStream()
        try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
    }
    try {
        $response = $request.GetResponse()
        try {
            if ([int]$response.StatusCode -ne 200 -or $response.ContentLength -gt $Maximum) { throw 'HTTP response rejected' }
            $stream = $response.GetResponseStream()
            $memory = [IO.MemoryStream]::new()
            try {
                $buffer = [byte[]]::new(8192)
                while (($count = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    if ($timer.Elapsed.TotalSeconds -gt 10) { throw 'HTTP deadline exceeded' }
                    if ($memory.Length + $count -gt $Maximum) { throw 'HTTP response too large' }
                    $memory.Write($buffer, 0, $count)
                }
                return ,$memory.ToArray()
            } finally { $memory.Dispose(); $stream.Dispose() }
        } finally { $response.Dispose() }
    } catch [Net.WebException] {
        $httpError = $_.Exception
        while ($httpError -isnot [Net.WebException] -and $null -ne $httpError.InnerException) { $httpError = $httpError.InnerException }
        if ($null -ne $httpError.Response) {
            $failure = [InvalidOperationException]::new('Agent update HTTP request failed', $httpError)
            try { $failure.Data['AgentUpdateHttpStatus'] = [int]$httpError.Response.StatusCode }
            finally { $httpError.Response.Dispose() }
            throw $failure
        }
        throw
    } finally { $request.Abort() }
}

function Get-AgentUpdateIdentity([string]$Root, [string]$Base, [string]$Device, [string]$Client, [switch]$Refresh) {
    $path = Join-Path $Root 'agent-update-identity.json'
    Assert-AgentPlainPath $path
    if (-not $Refresh -and (Test-Path -LiteralPath $path -PathType Leaf)) {
        try {
            $cached = [IO.File]::ReadAllText($path) | ConvertFrom-Json
            if ($cached.device -ieq $Device -and $cached.secret -cmatch '\A[0-9a-f]{64}\z') { return $cached }
        } catch { }
    }
    $bytes = Invoke-AgentUpdateRequest "$Base/agent-update-enroll" "client=$Client&account=53197668" 4096
    $identity = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
    if ($identity.device -ine $Device -or $identity.secret -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Enrollment rejected: identity mismatch' }
    # Restrict inheritance before writing either the secret or its atomic-write sibling.
    $acl = [IO.Directory]::GetAccessControl($Root, [Security.AccessControl.AccessControlSections]::Access)
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($access in @($acl.Access)) { [void]$acl.RemoveAccessRuleAll($access) }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
    [IO.Directory]::SetAccessControl($Root, $acl)
    Write-AgentDurable $path ([Text.Encoding]::UTF8.GetBytes((@{ device = $Device.ToUpperInvariant(); secret = $identity.secret } | ConvertTo-Json -Compress)))
    $identity
}

function Get-AgentManagementClient {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes("$env:COMPUTERNAME-$env:USERNAME")),0,4).Replace('-','') }
    finally { $sha.Dispose() }
}

function Get-AgentHubBinding([string]$Root) {
    $path = Join-Path $Root 'hub-management.json'
    Assert-AgentPlainPath $path
    if (-not (Test-Path -LiteralPath $path)) {
        if (Test-Path -LiteralPath (Join-Path $Root '.hub-management-required')) { throw 'Pinned hub binding is missing. Import the invitation again.' }
        return
    }
    try {
        $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $acl = [IO.File]::GetAccessControl($path)
        if (-not $acl.AreAccessRulesProtected -or -not $acl.GetOwner([Security.Principal.SecurityIdentifier]).Equals($sid) -or
            @($acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]) | Where-Object { -not $_.IdentityReference.Equals($sid) }).Count) { throw 'ACL' }
        if ((Get-Item -LiteralPath $path).Length -gt 4096) { throw 'Size' }
        $b = [IO.File]::ReadAllText($path) | ConvertFrom-Json -ErrorAction Stop
        $ip = $null
        if (($b.PSObject.Properties.Name | Sort-Object) -join ',' -cne 'Account,client,Device,Host,HttpPort,secret' -or
            $b.Host -isnot [string] -or -not [Net.IPAddress]::TryParse($b.Host,[ref]$ip) -or $ip.AddressFamily -ne 'InterNetwork' -or
            $ip.ToString() -cne $b.Host -or $b.Host -notmatch '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.|127\.)' -or
            ($b.HttpPort -isnot [int] -and $b.HttpPort -isnot [long]) -or $b.HttpPort -lt 1 -or $b.HttpPort -gt 65535 -or
            $b.Device -isnot [string] -or $b.Device -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or
            $b.Account -isnot [string] -or $b.Account -cne '53197668' -or $b.secret -isnot [string] -or $b.secret -cnotmatch '\A[0-9a-f]{64}\z' -or
            $b.client -isnot [string] -or $b.client -cne (Get-AgentManagementClient)) { throw 'Fields' }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $scope = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes(
            "$($b.Host):$($b.HttpPort)/$($b.Device.ToUpperInvariant())/$($b.client)/$($b.secret)"))).Replace('-','') }
        finally { $sha.Dispose() }
        $b | Add-Member NoteProperty Scope $scope
        return $b
    } catch { throw 'Invalid private hub-management.json binding.' }
}

function Open-AgentManagementGuard([string]$Root) {
    $path = Join-Path $Root '.management-binding.lock'
    Assert-AgentPlainPath $path
    [IO.File]::Open($path,'OpenOrCreate','ReadWrite','ReadWrite')
}

function Open-AgentPollLock([string]$Root) {
    $path = Join-Path $Root '.management-poll.lock'
    Assert-AgentPlainPath $path
    [IO.File]::Open($path,'OpenOrCreate','ReadWrite','None')
}

function Get-AgentManagementEndpoint([string]$Root) {
    $binding = Get-AgentHubBinding $Root
    if ($binding) { return $binding }
    $path = Join-Path $Root 'controller-endpoint.json'
    Assert-AgentPlainPath $path
    [IO.File]::ReadAllText($path) | ConvertFrom-Json
}

function Get-AgentRemovalContext([string]$Root,$State) {
    $endpoint = Get-AgentManagementEndpoint $Root
    if ($State.device -ine $endpoint.Device -or "$($State.scope)" -cne "$($endpoint.Scope)") { throw 'Management binding changed' }
    $identity = if ($endpoint.Scope) { $endpoint } else {
        $path = Join-Path $Root 'agent-update-identity.json'
        Assert-AgentPlainPath $path
        [IO.File]::ReadAllText($path) | ConvertFrom-Json
    }
    $version = if ($State.PSObject.Properties['version']) { $State.version } else { Get-AgentInstalledVersion $Root }
    [pscustomobject]@{Version=$version;Endpoint=$endpoint;Identity=$identity}
}

function Save-AgentRemoteUpdate([string]$Root, [string]$Stage, $Context = $null) {
    $guard = Open-AgentManagementGuard $Root
    try {
        $pollLock = Open-AgentPollLock $Root
        try { Invoke-AgentBoundPoll $Root $Stage $Context } finally { $pollLock.Dispose() }
    } finally { $guard.Dispose() }
}

function Invoke-AgentBoundPoll([string]$Root, [string]$Stage, $Context) {
    $random = [byte[]]::new(16)
    $rng = [Security.Cryptography.RNGCryptoServiceProvider]::new()
    try { $rng.GetBytes($random) } finally { $rng.Dispose() }
    $clientNonce = -join ($random | ForEach-Object { $_.ToString('x2') })
    $version = if ($null -ne $Context) { $Context.Version } else { Get-AgentInstalledVersion $Root }
    $hub = Get-AgentHubBinding $Root
    if ($null -ne $Context -and "$($Context.Endpoint.Scope)" -cne "$($hub.Scope)") { throw 'Stale management context' }
    $endpoint = if ($hub) { $hub } elseif ($null -ne $Context) { $Context.Endpoint } else { Get-AgentManagementEndpoint $Root }
    $ip = $null
    if ($endpoint.Device -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or $endpoint.Account -cne '53197668' -or
        -not [Net.IPAddress]::TryParse([string]$endpoint.Host, [ref]$ip) -or
        $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        [string]$endpoint.HttpPort -notmatch '\A[0-9]+\z' -or $endpoint.HttpPort -lt 1 -or $endpoint.HttpPort -gt 65535) { throw 'Pinned endpoint required' }
    $base = "http://$($ip.ToString()):$($endpoint.HttpPort)"
    $client = Get-AgentManagementClient
    $identity = if ($hub) { $hub } elseif ($null -ne $Context) { $Context.Identity } else { Get-AgentUpdateIdentity $Root $base $endpoint.Device $client }
    for ($attempt = 0; $attempt -lt 2; $attempt++) {
        try {
            $challengeBytes = Invoke-AgentUpdateRequest "$base/agent-update-challenge" "client=$client&account=53197668" 4096
            $challenge = [Text.Encoding]::UTF8.GetString($challengeBytes) | ConvertFrom-Json
            if ($challenge.device -ine $endpoint.Device -or $challenge.nonce -cnotmatch '\A[0-9a-f]{32}\z') { throw 'Controller challenge rejected' }
            $nonce = $challenge.nonce + $clientNonce
            $failedPath = Join-Path $Root '.agent-update-failed-version.txt'
            Assert-AgentPlainPath $failedPath
            $failed = 0L
            if (Test-Path -LiteralPath $failedPath) {
                $failedText = [IO.File]::ReadAllText($failedPath).Trim()
                if ($failedText -cnotmatch '\A[0-9]+\z') { throw 'Invalid failed release state' }
                $recorded = ConvertTo-AgentVersion ([decimal]$failedText)
                # A successful committed release is not a failure report.
                if ($recorded -gt $version) { $failed = $recorded }
            }
            $failureMessage = if ($failed -gt 0) { "FAILED`n$failed`n" } else { '' }
            $failureField = if ($failed -gt 0) { "&failed=$failed" } else { '' }
            $actionState = Read-AgentActionState $Root
            $ack = $null
            if ($null -ne $actionState -and -not $actionState.confirmed -and $actionState.device -ieq $endpoint.Device -and
                "$($actionState.scope)" -ceq "$($hub.Scope)" -and $actionState.result -in @('ok', 'failed')) { $ack = $actionState }
            $managementMessage = "MANAGE`n1`n"
            $managementFields = '&manage=1'
            if ($null -ne $ack) {
                $managementMessage += "ACK`n$($ack.action_id)`n$($ack.result)`n"
                $managementFields += "&ack=$($ack.action_id)&result=$($ack.result)"
            }
            $hmac = [Security.Cryptography.HMACSHA256]::new((ConvertFrom-AgentHex $identity.secret))
            try { $requestMac = -join ($hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("LFAU1-POLL`n$($endpoint.Device.ToUpperInvariant())`n$client`n$nonce`n$version`n$failureMessage$managementMessage")) | ForEach-Object { $_.ToString('x2') }) }
            finally { $hmac.Dispose() }
            $poll = Invoke-AgentUpdateRequest "$base/agent-update-poll" "client=$client&version=$version&account=53197668&nonce=$nonce&mac=$requestMac$failureField$managementFields" 4096
            $reply = [Text.Encoding]::UTF8.GetString($poll) | ConvertFrom-Json
            Test-AgentPollAuthentication $reply $endpoint.Device $client $nonce $identity.secret
            $active = Get-AgentHubBinding $Root
            if ("$($active.Scope)" -cne "$($hub.Scope)") { throw 'Management binding changed during poll' }
            if ($null -ne $ack) {
                $current = Read-AgentActionState $Root
                if ($null -eq $current -or $current.action_id -ne $ack.action_id -or $current.result -cne $ack.result) {
                    throw 'Management acknowledgment superseded'
                }
                $ack.confirmed = $true
                Write-AgentActionState $Root $ack
            }
            if ($null -ne $Context) { return $false }
            Save-AgentPollAction $Root $reply $endpoint.Device
            break
        } catch {
            if ($hub -or $null -ne $Context -or $attempt -ne 0 -or $_.Exception.Data['AgentUpdateHttpStatus'] -ne 403) { throw }
            $identity = Get-AgentUpdateIdentity $Root $base $endpoint.Device $client -Refresh
        }
    }
    $pendingAction = Read-AgentActionState $Root
    if ($null -ne $pendingAction -and -not $pendingAction.confirmed) { return $false }
    if ($reply.version -eq 0) { return $false }
    $target = ConvertTo-AgentVersion $reply.version
    if ($target -le $version) { return $false }
    if ($failed -gt 0 -and $target -le $failed) { return $false }
    Assert-AgentPlainPath $Stage
    [void][IO.Directory]::CreateDirectory($Stage)
    Write-AgentDurable (Join-Path $Stage 'manifest.json') (Invoke-AgentUpdateRequest "$base/agent-update/manifest.json" '' 8192)
    Write-AgentDurable (Join-Path $Stage 'package.zip') (Invoke-AgentUpdateRequest "$base/agent-update/package.zip" '' 524288)
    $package = Test-AgentPackage $Root $Stage $version
    if ($package.Version -ne $target) { throw 'Target version mismatch' }
    Write-AgentDurable (Join-Path $Stage '.management-scope') ([Text.Encoding]::ASCII.GetBytes([string]$hub.Scope))
    $true
}

function ConvertFrom-AgentHex([string]$Hex) {
    if ($Hex -cnotmatch '\A(?:[0-9a-f]{2})+\z') { throw 'Invalid hex' }
    $bytes = [byte[]]::new($Hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16) }
    return ,$bytes
}

function Test-AgentPollAuthentication($Reply, [string]$Device, [string]$Client, [string]$Nonce, [string]$Secret) {
    if ($Reply.device -ine $Device -or $Reply.nonce -cne $Nonce -or $Reply.mac -cnotmatch '\A[0-9a-f]{64}\z' -or
        $Reply.version -is [string] -or $Reply.version -is [bool] -or [string]$Reply.version -cnotmatch '\A[0-9]+\z' -or
        [decimal]$Reply.version -gt 9007199254740991) { throw 'Unauthenticated poll response' }
    $hmac = [Security.Cryptography.HMACSHA256]::new((ConvertFrom-AgentHex $Secret))
    $actionMessage = Get-AgentActionMessage $Reply
    try { $expected = $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("LFAU1`n$($Device.ToUpperInvariant())`n$($Client.ToUpperInvariant())`n$Nonce`n$($Reply.version)`n$actionMessage")) }
    finally { $hmac.Dispose() }
    $actual = ConvertFrom-AgentHex $Reply.mac
    $difference = 0
    for ($i = 0; $i -lt 32; $i++) { $difference = $difference -bor ($expected[$i] -bxor $actual[$i]) }
    if ($difference -ne 0) { throw 'Unauthenticated poll MAC' }
}

function Get-AgentActionMessage($Reply) {
    $fields = @($Reply.PSObject.Properties.Name | Where-Object { $_ -in @('action', 'action_id', 'name_hex') })
    if ($fields.Count -eq 0) { return '' }
    if ($fields.Count -ne 3 -or $Reply.action -cnotin @('rename', 'uninstall') -or $Reply.name_hex -isnot [string]) { throw 'Invalid action fields' }
    $id = ConvertTo-AgentVersion $Reply.action_id
    if ($Reply.action -ceq 'uninstall') {
        if ($Reply.name_hex -cne '') { throw 'Uninstall name must be empty' }
    } else {
        if ($Reply.name_hex -cnotmatch '\A(?:[0-9a-f]{2}){1,48}\z') { throw 'Invalid name hex' }
        $name = [Text.UTF8Encoding]::new($false, $true).GetString((ConvertFrom-AgentHex $Reply.name_hex))
        if ([string]::IsNullOrWhiteSpace($name)) { throw 'Empty name' }
        foreach ($character in $name.ToCharArray()) { if ([char]::IsControl($character)) { throw 'Control character in name' } }
    }
    "ACTION`n$id`n$($Reply.action)`n$($Reply.name_hex)`n"
}

function Get-AgentActionPath([string]$Root) {
    $b = Get-AgentHubBinding $Root
    if ($b) { return (Join-Path $Root ('.hub-action-' + $b.Scope + '.json')) }
    Join-Path $Root '.agent-action.json'
}
function Read-AgentActionState([string]$Root) {
    try { $path = Get-AgentActionPath $Root } catch { return }
    Assert-AgentPlainPath $path
    if (-not [IO.File]::Exists($path)) { return $null }
    if ((Get-Item -LiteralPath $path).Length -gt 4096) { throw 'Action state too large' }
    $state = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    [void](Get-AgentActionMessage $state)
    if ($state.device -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or $state.result -cnotin @('pending', 'ok', 'failed') -or $state.confirmed -isnot [bool]) { throw 'Invalid action state' }
    $state
}

function Write-AgentActionState([string]$Root, $State) {
    $active = Get-AgentHubBinding $Root
    if ("$($State.scope)" -cne "$($active.Scope)") { throw 'Stale management action' }
    Write-AgentDurable (Get-AgentActionPath $Root) ([Text.Encoding]::UTF8.GetBytes(($State | ConvertTo-Json -Compress)))
}

function Reset-AgentActionForReinstall([string]$Root) {
    $state = Read-AgentActionState $Root
    if ($null -eq $state -or $state.action -ne 'uninstall' -or $state.result -ne 'ok') { return }
    $path = Get-AgentActionPath $Root
    Assert-AgentPlainPath "$path.new"
    [IO.File]::Delete($path)
    [IO.File]::Delete("$path.new")
}

function Save-AgentPollAction([string]$Root, $Reply, [string]$Device) {
    if (-not (Get-AgentActionMessage $Reply)) { return }
    if ($Reply.version -ne 0) { throw 'Action concurrent with update' }
    $previous = Read-AgentActionState $Root
    if ($null -ne $previous -and $previous.device -ieq $Device) {
        if ($previous.action_id -eq $Reply.action_id) {
            if ($previous.action -cne $Reply.action -or $previous.name_hex -cne $Reply.name_hex) { throw 'Action ID reused with different content' }
            return
        }
        # IDs have a random boot prefix: ordering across controller boots is undefined.
        if (-not $previous.confirmed -or ($previous.action -eq 'uninstall' -and $previous.result -eq 'ok')) { return }
    }
    $received = [DateTime]::UtcNow
    $binding = Get-AgentHubBinding $Root
    Write-AgentActionState $Root ([pscustomobject]@{ scope = [string]$binding.Scope; device = $Device; action_id = $Reply.action_id; action = $Reply.action; name_hex = $Reply.name_hex; result = 'pending'; confirmed = $false; received_utc = $received.ToString('o'); expires_utc = $received.AddMinutes(10).ToString('o') })
}

function Rename-AgentMetadata([string]$Root, $Action) {
    [void](Get-AgentActionMessage $Action)
    $path = Join-Path $Root 'agent-metadata.json'
    Assert-AgentPlainPath $path
    if (-not [IO.File]::Exists($path)) { throw 'Missing metadata' }
    if ((Get-Item -LiteralPath $path).Length -gt 65536) { throw 'Metadata too large' }
    # The dictionary serializer retains unknown fields, nested objects and integer values on PS 5.1.
    Add-Type -AssemblyName System.Web.Extensions
    $serializer = [Web.Script.Serialization.JavaScriptSerializer]::new()
    $serializer.MaxJsonLength = 65536
    $serializer.RecursionLimit = 100
    $metadata = $serializer.DeserializeObject([IO.File]::ReadAllText($path))
    if ($metadata -isnot [Collections.Generic.Dictionary[string,object]]) { throw 'Metadata must be an object' }
    $name = [Text.UTF8Encoding]::new($false, $true).GetString((ConvertFrom-AgentHex $Action.name_hex))
    $metadata['Name'] = $name
    Write-AgentDurable $path ([Text.Encoding]::UTF8.GetBytes($serializer.Serialize($metadata)))
}

function Assert-AgentUninstallRoot([string]$Root) {
    $expected = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'LED Forge/Codex LAN Agent'))
    if (-not [string]::Equals([IO.Path]::GetFullPath($Root).TrimEnd('\'), $expected.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Uninstall requires exact installed root' }
    Assert-AgentPlainPath $Root
}

function Assert-AgentSupervisorLaunchArguments([string]$Root, [string]$Arguments) {
    $scriptPath = Join-Path $Root 'start_agent_supervisor.ps1'
    if ($Arguments.Contains('%') -or $Arguments -match '[\r\n]') { throw 'Invalid supervisor launch arguments' }
    $matches = [regex]::Matches($Arguments, '"[^"\r\n]*"|[^\s"]+')
    $tokens = @(); $end = 0
    foreach ($match in $matches) {
        if ($end -gt 0 -and $match.Index -eq $end) { throw 'Adjacent launch tokens require whitespace' }
        if ($Arguments.Substring($end, $match.Index - $end).Trim()) { throw 'Invalid launch quoting' }
        $tokens += $match.Value.Trim('"'); $end = $match.Index + $match.Length
    }
    if ($Arguments.Substring($end).Trim()) { throw 'Invalid launch quoting' }
    $seen = @{}; $index = 0; $fileFound = $false
    while ($index -lt $tokens.Count) {
        $name = $tokens[$index++].ToLowerInvariant()
        if ($seen.ContainsKey($name)) { throw 'Duplicate launch argument' }
        $seen[$name] = $true
        if ($name -eq '-file') {
            if ($index -ge $tokens.Count -or $tokens[$index++] -ine $scriptPath) { throw 'Foreign supervisor script' }
            $fileFound = $true; break
        }
        if ($name -in @('-noprofile', '-noninteractive')) { continue }
        if ($name -eq '-executionpolicy' -and $index -lt $tokens.Count -and $tokens[$index++] -ieq 'Bypass') { continue }
        if ($name -eq '-windowstyle' -and $index -lt $tokens.Count -and $tokens[$index++] -ieq 'Hidden') { continue }
        throw 'Unknown PowerShell launch argument'
    }
    if (-not $fileFound) { throw 'Missing fixed supervisor script' }
    while ($index -lt $tokens.Count) {
        $name = $tokens[$index++].ToLowerInvariant()
        if ($seen.ContainsKey($name) -or $index -ge $tokens.Count) { throw 'Invalid supervisor argument' }
        $seen[$name] = $true; $value = $tokens[$index++]
        switch ($name) {
            { $_ -in @('-collectorhost', '-directhost') } {
                if ($value -cnotmatch '\A[a-zA-Z0-9._:-]+\z') { throw 'Invalid launch host' }
            }
            { $_ -in @('-port', '-directport', '-directhttpport') } {
                if ($value -cnotmatch '\A[0-9]{1,5}\z' -or [int]$value -lt 1 -or [int]$value -gt 65535) { throw 'Invalid launch port' }
            }
            '-instancename' { if ($value -cnotmatch '\A[a-zA-Z0-9_-]+\z') { throw 'Invalid launch instance' } }
            '-logpath' {
                if (-not [IO.Path]::IsPathRooted($value) -or $value.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0 -or $value.EndsWith('\')) { throw 'Invalid launch log path' }
            }
            default { throw 'Unknown supervisor launch argument' }
        }
    }
}

function Get-AgentGuiLauncherContent([string]$Root, [string]$Arguments) {
    Assert-AgentSupervisorLaunchArguments $Root $Arguments
    if ($Root.Contains('"') -or $Root.Contains('%') -or $Root -match '[\r\n]') { throw 'Invalid launcher root' }
    $exe = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $command = ('"' + $exe + '" ' + $Arguments).Replace('"', '""')
    $directory = $Root.Replace('"', '""')
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Arguments))
    # This fixed wrapper is derived from signed code, never an arbitrary script.
    @(
        'Option Explicit'
        ("' AgentArguments=" + $encoded)
        'If WScript.Arguments.Count <> 0 Then WScript.Quit 64'
        'Dim shell, code'
        'Set shell = CreateObject("WScript.Shell")'
        ('shell.CurrentDirectory = "' + $directory + '"')
        ('code = shell.Run("' + $command + '", 0, True)')
        'WScript.Quit code'
    ) -join "`r`n"
}

function Get-AgentGuiLauncherArguments([string]$Root) {
    '//B //Nologo "' + (Join-Path $Root 'start_agent_hidden.vbs') + '"'
}

function Assert-AgentGuiLauncher([string]$Root) {
    $path = Join-Path $Root 'start_agent_hidden.vbs'
    Assert-AgentPlainPath $path
    $content = [IO.File]::ReadAllText($path)
    $match = [regex]::Match($content, "(?m)^' AgentArguments=([A-Za-z0-9+/=]+)\r?$")
    if (-not $match.Success) { throw 'Unknown GUI launcher content' }
    $arguments = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($match.Groups[1].Value))
    if ($content -cne (Get-AgentGuiLauncherContent $Root $arguments)) { throw 'Unknown GUI launcher content' }
}

function Write-AgentGuiLauncher([string]$Root, [string]$Arguments) {
    $content = Get-AgentGuiLauncherContent $Root $Arguments
    # UTF-16LE is understood by WSH on Windows, including Unicode user paths.
    $encoding = [Text.UnicodeEncoding]::new($false, $true)
    $bytes = $encoding.GetPreamble() + $encoding.GetBytes($content)
    Write-AgentDurable (Join-Path $Root 'start_agent_hidden.vbs') $bytes
}

function Assert-AgentAutostartOwner([string]$Root, $Shortcut) {
    if (-not [string]::Equals(([string]$Shortcut.WorkingDirectory).TrimEnd('\'), $Root.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Autostart belongs to another installation' }
    $legacy = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $gui = Join-Path $env:SystemRoot 'System32/wscript.exe'
    if ([string]::Equals($Shortcut.TargetPath, $legacy, [StringComparison]::OrdinalIgnoreCase)) {
        Assert-AgentSupervisorLaunchArguments $Root ([string]$Shortcut.Arguments)
        return
    }
    if (-not [string]::Equals($Shortcut.TargetPath, $gui, [StringComparison]::OrdinalIgnoreCase) -or
        $Shortcut.Arguments -cne (Get-AgentGuiLauncherArguments $Root)) { throw 'Autostart belongs to another installation' }
    Assert-AgentGuiLauncher $Root
}

function Assert-AgentStartupShortcutOwner([string]$Root, [string]$Path) {
    Assert-AgentPlainPath $Path
    if (-not [IO.File]::Exists($Path)) { return }
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($Path)
        try { Assert-AgentAutostartOwner $Root $shortcut }
        finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shortcut) }
    } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }
}

function Repair-AgentGuiRecoveryTask([string]$Root) {
    # Development/test copies and unrelated installations must never query tasks.
    $installed = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'LED Forge/Codex LAN Agent')).TrimEnd('\')
    if (-not [string]::Equals([IO.Path]::GetFullPath($Root).TrimEnd('\'), $installed, [StringComparison]::OrdinalIgnoreCase)) { return }
    $task = Get-AgentRecoveryTask
    if ($null -eq $task) { return }
    Assert-AgentRecoveryTaskOwner $Root $task
    $legacy = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    if ([string]::Equals($task.Actions[0].Execute, $legacy, [StringComparison]::OrdinalIgnoreCase)) {
        Register-AgentRecoveryTask $Root ([string]$task.Actions[0].Arguments)
    }
}

function Get-AgentRecoveryTaskName {
    'Codex LAN Agent Recovery-' + [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
}

function Get-AgentRecoveryTask {
    $name = Get-AgentRecoveryTaskName
    Get-ScheduledTask -TaskPath '\' -ErrorAction Stop | Where-Object TaskName -eq $name
}

function Assert-AgentRecoveryTaskOwner([string]$Root, $Task) {
    if ($Task.TaskName -ne (Get-AgentRecoveryTaskName) -or $Task.TaskPath -ne '\') { throw 'Unexpected recovery task identity' }
    $actions = @($Task.Actions)
    if ($actions.Count -ne 1) { throw 'Recovery task has unexpected actions' }
    $action = $actions[0]
    Assert-AgentAutostartOwner $Root ([pscustomobject]@{
        TargetPath = $action.Execute; Arguments = $action.Arguments; WorkingDirectory = $action.WorkingDirectory
    })
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $user = [string]$Task.Principal.UserId
    if ($user -ne $sid) {
        try { $user = ([Security.Principal.NTAccount]::new($user)).Translate([Security.Principal.SecurityIdentifier]).Value }
        catch { throw 'Cannot verify recovery task owner' }
    }
    if ($user -ne $sid) { throw 'Recovery task belongs to another user' }
}

function Register-AgentRecoveryTask([string]$Root, [string]$Arguments) {
    Assert-AgentUninstallRoot $Root
    $exe = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    Assert-AgentAutostartOwner $Root ([pscustomobject]@{
        TargetPath = $exe; Arguments = $Arguments; WorkingDirectory = $Root
    })
    $existing = Get-AgentRecoveryTask
    if ($null -ne $existing) { Assert-AgentRecoveryTaskOwner $Root $existing }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    Write-AgentGuiLauncher $Root $Arguments
    $guiExe = Join-Path $env:SystemRoot 'System32/wscript.exe'
    $action = New-ScheduledTaskAction -Execute $guiExe -Argument (Get-AgentGuiLauncherArguments $Root) -WorkingDirectory $Root
    $triggers = @(
        New-ScheduledTaskTrigger -AtLogOn -User $sid
        New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 1)
    )
    # IgnoreNew plus the supervisor mutex prevent duplicate workers, including update handoffs.
    $settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $principal = New-ScheduledTaskPrincipal -UserId $sid -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName (Get-AgentRecoveryTaskName) -TaskPath '\' -Action $action -Trigger $triggers -Settings $settings -Principal $principal -Description 'Restore the local Codex agent within one minute while its Windows user is signed in.' -Force -ErrorAction Stop | Out-Null
}

function Remove-AgentRecoveryTask([string]$Root) {
    Assert-AgentUninstallRoot $Root
    $task = Get-AgentRecoveryTask
    if ($null -eq $task) { return }
    Assert-AgentRecoveryTaskOwner $Root $task
    Unregister-ScheduledTask -TaskName $task.TaskName -TaskPath '\' -Confirm:$false -ErrorAction Stop
}

function Remove-AgentStartupShortcut([string]$Root) {
    Assert-AgentUninstallRoot $Root
    $path = Join-Path ([Environment]::GetFolderPath('Startup')) 'LED Forge Codex LAN Agent.lnk'
    Assert-AgentPlainPath $path
    if (-not [IO.File]::Exists($path)) { return }
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($path)
        try { Assert-AgentAutostartOwner $Root $shortcut
        } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shortcut) }
    } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }
    [IO.File]::Delete($path)
}

function Remove-AgentAutostart([string]$Root) {
    Remove-AgentRecoveryTask $Root
    Remove-AgentStartupShortcut $Root
}

function Remove-AgentFixedCode([string]$Root) {
    Assert-AgentUninstallRoot $Root
    $paths = @(@(Get-AgentCodeFiles) + @('start_agent_hidden.vbs') | ForEach-Object { Join-Path $Root $_ })
    # Preflight the complete set before touching any file; never enumerate or recurse into state.
    foreach ($path in $paths) { Assert-AgentPlainPath $path; Assert-AgentPlainPath "$path.new" }
    foreach ($path in $paths) { [IO.File]::Delete($path); [IO.File]::Delete("$path.new") }
}

function Stop-AgentManagedWorker($Worker, [string]$Control) {
    if ($null -eq $Worker -or $Worker.HasExited) { return }
    Write-AgentDurable "$Control.stop" ([byte[]]@(1))
    if (-not $Worker.WaitForExit(15000)) { throw 'Worker did not stop gracefully; action deferred' }
}

function Invoke-AgentPendingAction([string]$Root, $Worker, [string]$Control) {
    $guard = Open-AgentManagementGuard $Root
    try {
        $pollLock = Open-AgentPollLock $Root
        try { Invoke-AgentBoundAction $Root $Worker $Control } finally { $pollLock.Dispose() }
    } finally { $guard.Dispose() }
}

function Invoke-AgentBoundAction([string]$Root, $Worker, [string]$Control) {
    $state = Read-AgentActionState $Root
    if ($null -eq $state) { return '' }
    $endpoint = Get-AgentManagementEndpoint $Root
    if ($state.device -ine $endpoint.Device -or "$($state.scope)" -cne "$($endpoint.Scope)") { return '' }
    if ($state.action -eq 'uninstall' -and ($state.result -eq 'ok' -or ($state.result -eq 'failed' -and $state.removal_started))) { return 'uninstall' }
    if ($state.result -ne 'pending') { return '' }
    try {
        $received = [DateTime]::Parse([string]$state.received_utc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
        $expires = [DateTime]::Parse([string]$state.expires_utc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
        $now = [DateTime]::UtcNow
        $expired = $now -ge $expires -or $now -lt $received -or $expires -gt $received.AddMinutes(10)
    } catch { $expired = $true }
    if ($expired) {
        $state.result = 'failed'
        Write-AgentActionState $Root $state
        return ''
    }
    # A timed-out stop remains pending: never modify code or metadata while the worker writes state.
    Stop-AgentManagedWorker $Worker $Control
    if ([DateTime]::UtcNow -ge $expires) {
        $state.result = 'failed'
        Write-AgentActionState $Root $state
        return ''
    }
    try {
        if ($state.action -eq 'rename') { Rename-AgentMetadata $Root $state }
        else {
            $state | Add-Member NoteProperty removal_started $true -Force
            Write-AgentActionState $Root $state
            Remove-AgentAutostart $Root; Remove-AgentFixedCode $Root
        }
        $state.result = 'ok'
    } catch { $state.result = 'failed' }
    Write-AgentActionState $Root $state
    if ($state.action -eq 'uninstall') { return 'uninstall' }
    'restart'
}

function Wait-AgentUninstallAck([string]$Root, $Context, [string]$LoadedHelper,
    [ValidateRange(100, 60000)][int]$TimeoutMilliseconds = 60000) {
    # Functions and pinned context remain in memory after their fixed source files are removed.
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $workBudget = $TimeoutMilliseconds - [Math]::Min(5000, [int]($TimeoutMilliseconds / 2))
    while (-not (Read-AgentActionState $Root).confirmed -and $timer.ElapsedMilliseconds -lt $workBudget) {
        $job = [PowerShell]::Create()
        try {
            [void]$job.AddScript($LoadedHelper)
            [void]$job.AddScript('param($r,$c) $ErrorActionPreference="Stop"; Save-AgentRemoteUpdate $r "" $c').AddArgument($Root).AddArgument($Context)
            $async = $job.BeginInvoke()
            # Reserve five seconds for cancellation of the bounded four-second HTTP I/O.
            $budget = [int][Math]::Min(30000, [Math]::Max(1, $workBudget - $timer.ElapsedMilliseconds))
            if (-not $async.AsyncWaitHandle.WaitOne($budget)) { $job.Stop() }
            else { try { [void]$job.EndInvoke($async) } catch { } }
        } finally { $job.Dispose() }
        if (-not (Read-AgentActionState $Root).confirmed) {
            $delay = [int][Math]::Min(15000, [Math]::Max(0, $workBudget - $timer.ElapsedMilliseconds))
            if ($delay -gt 0) { Start-Sleep -Milliseconds $delay }
        }
    }
}
