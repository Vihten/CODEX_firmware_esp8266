# Windows PowerShell 5.1. Dot-source agent_update.ps1 for durable file operations.
$chatUtf8 = [Text.UTF8Encoding]::new($false, $true)
$chatUuid = '\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\z'

function Read-ChatObject([string]$Path) {
    Assert-AgentPlainPath $Path
    $stream = [IO.File]::Open($Path,'Open','Read',[IO.FileShare]'Read,Delete')
    try {
        if ($stream.Length -gt 16384) { throw 'Mailbox record too large' }
        $length = [int]$stream.Length
        $reader = [IO.BinaryReader]::new($stream)
        $bytes = $reader.ReadBytes($length)
        $chatUtf8.GetString($bytes).TrimStart([char]0xfeff) | ConvertFrom-Json -ErrorAction Stop
    } finally { $stream.Dispose() }
}

function Write-ChatObject($Path,$Value) {
    Write-AgentDurable $Path ($chatUtf8.GetBytes((ConvertTo-Json -InputObject $Value -Depth 5 -Compress)))
}

function Initialize-ChatMailbox($Root) {
    Assert-AgentPlainPath $Root
    [void][IO.Directory]::CreateDirectory($Root)
    $acl = [IO.Directory]::GetAccessControl($Root)
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    if (-not $acl.GetOwner([Security.Principal.SecurityIdentifier]).Equals($sid)) { throw 'Foreign mailbox' }
    $acl.SetAccessRuleProtection($true,$false)
    foreach ($rule in @($acl.Access)) { [void]$acl.RemoveAccessRuleAll($rule) }
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))
    [IO.Directory]::SetAccessControl($Root,$acl)
    foreach ($name in 'inbox','outbox','seen','attempts','failed') {
        $path = Join-Path $Root $name
        Assert-AgentPlainPath $path
        [void][IO.Directory]::CreateDirectory($path)
    }
}

function Get-ChatEndpoint($Path) {
    $ep = Read-ChatObject $Path
    $ip = $null
    if ($ep.Device -cnotmatch '\A[0-9A-Fa-f]{6}\z' -or $ep.Account -cne '53197668' -or
        -not [Net.IPAddress]::TryParse([string]$ep.Host,[ref]$ip) -or $ip.AddressFamily -ne 'InterNetwork' -or
        $ip.ToString() -cne $ep.Host -or [string]$ep.HttpPort -notmatch '\A[0-9]{1,5}\z' -or
        [int]$ep.HttpPort -lt 1 -or [int]$ep.HttpPort -gt 65535) { throw 'Invalid controller endpoint' }
    @{ Base="http://$($ip.ToString()):$($ep.HttpPort)"; Device=$ep.Device.ToUpperInvariant() }
}

function Get-ChatSecret($Root,$Device,$Client,[switch]$Create) {
    if ($Device -cnotmatch '\A[0-9A-F]{6}\z' -or $Client -cnotmatch '\A[0-9A-F]{8}\z') { throw 'Invalid pairing identity' }
    Add-Type -AssemblyName System.Security
    $path = Join-Path $Root "chat-key-$Device-$Client.bin"
    Assert-AgentPlainPath $path
    $entropy = $chatUtf8.GetBytes("LFCHAT1/$Device/$Client")
    if (-not [IO.File]::Exists($path)) {
        if (-not $Create) { throw 'Pairing required' }
        $key = [byte[]]::new(32)
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($key) } finally { $rng.Dispose() }
        $blob = [Security.Cryptography.ProtectedData]::Protect($key,$entropy,'CurrentUser')
        Write-AgentDurable $path $blob
    }
    if ((Get-Item -LiteralPath $path).Length -gt 4096) { throw 'Invalid protected key' }
    $key = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($path),$entropy,'CurrentUser')
    if ($key.Length -ne 32) { throw 'Invalid chat key' }
    return ,$key
}

function Get-ChatMac([byte[]]$Key,[string]$Value) {
    $h = [Security.Cryptography.HMACSHA256]::new($Key)
    try { [BitConverter]::ToString($h.ComputeHash($chatUtf8.GetBytes($Value))).Replace('-','').ToLowerInvariant() }
    finally { $h.Dispose() }
}

function Get-ChatSequence($Context) {
    if ($Context.Seq -ge $Context.SequenceLimit) {
        $path = Join-Path $Context.Root ("sequence-" + $Context.Device + "-" + $Context.Client + ".json")
        Assert-AgentPlainPath $path
        $last = if ([IO.File]::Exists($path)) { (Read-ChatObject $path).limit } else { 0L }
        if ([string]$last -cnotmatch '\A[0-9]{1,16}\z' -or [long]$last -gt 9007199254736895L) { throw 'Invalid sequence state' }
        $start = [Math]::Max([long]$last,[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        $limit = $start + 4096L
        Write-ChatObject $path @{limit=$limit}
        $Context.Seq = $start
        $Context.SequenceLimit = $limit
    }
    $Context.Seq = 1L + $Context.Seq
    $Context.Seq
}

function Invoke-ChatRest($Context,[ValidateSet('state','roster','send','inbox','ack')]$Operation,
        [string]$Id='', [string]$To='', [string]$Body='') {
    if ($Context.Stop -and $Context.Stop.WaitOne(0)) { throw 'Chat session stopping' }
    $path = "/chat/$Operation"
    $request = [Net.HttpWebRequest]::Create($Context.Base + $path)
    $request.Proxy = $null
    $request.AllowAutoRedirect = $false
    $request.Method = 'POST'
    $request.Timeout = 2500
    $request.ReadWriteTimeout = 2000
    $request.MaximumResponseHeadersLength = 8
    $request.KeepAlive = $false
    $request.ContentType = 'text/plain; charset=utf-8'
    $bytes = $chatUtf8.GetBytes($Body)
    if ($bytes.Length -gt 512) { throw 'Oversize chat body' }
    $request.ContentLength = $bytes.Length
    if ($Operation -ne 'state') {
        $seq = Get-ChatSequence $Context
        $client = $Context.Client
        $boot = $Context.Boot
        $request.Headers['X-Chat-Client'] = $client
        $request.Headers['X-Chat-Seq'] = [string]$seq
        $request.Headers['X-Chat-Mac'] = Get-ChatMac $Context.Key ("LFCHAT1",$boot,$client,$seq,$path,$Id,$To,$Body -join [char]10)
        if ($Id) { $request.Headers['X-Chat-Id'] = $Id }
        if ($To) { $request.Headers['X-Chat-To'] = $To }
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        if ($bytes.Length) {
            $s = $request.GetRequestStream()
            try { $s.Write($bytes,0,$bytes.Length) } finally { $s.Dispose() }
        }
        $response = $request.GetResponse()
        try {
            if ([int]$response.StatusCode -ne 200 -or $response.ContentLength -gt 8192) { throw 'Invalid chat response' }
            $stream = $response.GetResponseStream()
            $memory = [IO.MemoryStream]::new()
            try {
                $buffer = [byte[]]::new(1024)
                while (($count = $stream.Read($buffer,0,$buffer.Length)) -gt 0) {
                    if ($timer.ElapsedMilliseconds -gt 4500 -or $memory.Length + $count -gt 8192) { throw 'Chat response limit' }
                    $memory.Write($buffer,0,$count)
                }
                $json = $chatUtf8.GetString($memory.ToArray())
            } finally { $memory.Dispose(); $stream.Dispose() }
            if ($Operation -ne 'state') {
                $mac = Get-ChatMac $Context.Key ("LFCHAT1-REPLY",$boot,$client,$seq,$json -join [char]10)
                $actual = $response.Headers['X-Chat-Reply']
                if ($actual -cnotmatch '\A[0-9a-f]{64}\z') { throw 'Unsigned chat response' }
                $diff = 0
                for ($i=0; $i -lt 64; $i++) { $diff = $diff -bor ([int]$actual[$i] -bxor [int]$mac[$i]) }
                if ($diff) { throw 'Invalid chat response signature' }
            }
            ConvertFrom-Json -InputObject $json -ErrorAction Stop
        } finally { $response.Dispose() }
    } catch [Net.WebException] {
        $errorResponse = $_.Exception.Response
        if ($errorResponse) {
            $failure = [InvalidOperationException]::new('Chat request rejected')
            $failure.Data['Status'] = [int]$errorResponse.StatusCode
            $errorResponse.Dispose()
            throw $failure
        }
        throw
    } finally { $request.Abort() }
}

function Test-ChatText($Value) {
    $Value -is [string] -and $Value.Length -gt 0 -and $Value -notmatch '[\x00-\x09\x0b-\x1f\x7f]' -and $chatUtf8.GetByteCount($Value) -le 512
}

function Get-ChatRoster($Context) {
    $r = Invoke-ChatRest $Context roster
    if ($r.self_id -cne $Context.Client -or $r.agents -isnot [array] -or $r.agents.Count -gt 13) { throw 'Invalid chat roster' }
    $known = @{}
    foreach ($peer in $r.agents) {
        if ($peer.id -isnot [string] -or $peer.id -cnotmatch '\A([0-9A-F]{8}|board)\z' -or $known.ContainsKey($peer.id) -or
            $peer.online -isnot [bool] -or $peer.name -isnot [string] -or $peer.name.Length -gt 128) { throw 'Invalid roster member' }
        $known[$peer.id] = $true
    }
    $r
}

function Receive-ChatDelivery($Root,$Context) {
    $r = Invoke-ChatRest $Context inbox
    if ($r.messages -isnot [array] -or $r.messages.Count -gt 1) { throw 'Invalid chat inbox' }
    foreach ($msg in $r.messages) {
        if ($msg.id -isnot [string] -or $msg.id -cnotmatch $chatUuid -or $msg.sender_id -cnotmatch '\A[0-9A-F]{8}\z' -or
            $msg.sender_name -isnot [string] -or $msg.sender_name.Length -gt 128 -or -not (Test-ChatText $msg.text)) { throw 'Invalid delivery' }
        $path = Join-Path $Root "inbox/$($msg.id).json"
        if (-not [IO.File]::Exists($path) -and -not [IO.File]::Exists((Join-Path $Root "seen/$($msg.id).json"))) {
            if (@([IO.Directory]::EnumerateFiles((Join-Path $Root 'inbox'),'*.json') | Select-Object -First 64).Count -ge 64) {
                throw 'Local inbox full'
            }
            Write-ChatObject $path $msg
        }
        [void](Invoke-ChatRest $Context ack $msg.id)
    }
}

function Send-ChatOutbox($Root,$Context,$Roster) {
    $dir = Join-Path $Root 'outbox'
    foreach ($path in [IO.Directory]::EnumerateFiles($dir,'*.json') | Select-Object -First 4) {
        $id = [IO.Path]::GetFileNameWithoutExtension($path)
        Assert-AgentPlainPath $path
        $attempt = $null
        try {
            if ($id -cnotmatch $chatUuid) { throw 'Invalid filename' }
            $msg = Read-ChatObject $path
            if ($msg.id -cne $id -or -not (Test-ChatText $msg.text) -or $msg.recipient_ids -isnot [array] -or
                $msg.recipient_ids.Count -lt 1 -or $msg.recipient_ids.Count -gt 12) { throw 'Invalid queued message' }
            $attempt = Join-Path $Root "attempts/$id.json"
            $retry = [IO.File]::Exists($attempt)
            $recipients = @{}
            foreach ($to in $msg.recipient_ids) {
                if ($to -isnot [string] -or $to -cnotmatch '\A([0-9A-F]{8}|board)\z' -or $to -ceq $Context.Client -or
                    $recipients.ContainsKey($to) -or (-not $retry -and -not ($Roster.agents | Where-Object { $_.online -and $_.id -ceq $to }))) { throw 'Invalid recipient' }
                $recipients[$to] = $true
            }
            $toList = $msg.recipient_ids -join ','
            $digest = Get-ChatMac $Context.Key ($toList + [char]10 + $msg.text)
            $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
            if ($retry) {
                $record = Read-ChatObject $attempt
                if ($record.boot -cne $Context.Boot -or $record.digest -cne $digest -or
                    $now -lt $record.started -or $now - $record.started -ge 590) { throw 'Expired or changed attempt' }
            }
        } catch {
            $failed = Join-Path $Root ('failed/' + [IO.Path]::GetFileName($path))
            Assert-AgentPlainPath $failed
            if (-not [IO.File]::Exists($failed)) { [IO.File]::Move($path,$failed) }
            if ($attempt) { [IO.File]::Delete($attempt) }
            continue
        }
        if (-not $retry) { Write-ChatObject $attempt @{boot=$Context.Boot;digest=$digest;started=$now} }
        try {
            $reply = Invoke-ChatRest $Context send $id $toList $msg.text
            if ($reply.ok -isnot [bool] -or -not $reply.ok) { throw 'Missing send receipt' }
        } catch {
            if ($_.Exception.Data['Status'] -notin @(400,409,413)) { throw }
            [IO.File]::Move($path,(Join-Path $Root "failed/$id.json"))
            [IO.File]::Delete($attempt)
            continue
        }
        [IO.File]::Delete($path)
        [IO.File]::Delete($attempt)
    }
}

function Publish-ChatRoster($Root,$Context,$Roster) {
    $json = ConvertTo-Json -InputObject $Roster -Depth 5 -Compress
    if ($json -cne $Context.LastSnapshot) {
        Write-AgentDurable (Join-Path $Root 'roster.json') ($chatUtf8.GetBytes($json))
        $Context.LastSnapshot = $json
    }
}

function Invoke-ChatCycle($Root,$EndpointPath,$Context) {
    $Context.Root = $Root
    $now = [datetime]::UtcNow
    if (-not $Context.Key -or $now -ge $Context.NextState) {
        $ep = Get-ChatEndpoint $EndpointPath
        if ($Context.Device -cne $ep.Device -or -not $Context.Key) {
            $Context.Key = Get-ChatSecret $Root $ep.Device $Context.Client
            $Context.Device = $ep.Device
            $Context.Seq = 0L
            $Context.SequenceLimit = 0L
        }
        $Context.Base = $ep.Base
        $state = Invoke-ChatRest $Context state
        if ($state.boot -isnot [string] -or $state.boot -cnotmatch '\A[0-9a-f]{16}\z') { throw 'Invalid chat boot' }
        if ($Context.Boot -cne $state.boot) { $Context.NextRoster = [datetime]::MinValue }
        $Context.Boot = $state.boot
        $Context.NextState = $now.AddSeconds((Get-Random -Minimum 90 -Maximum 121))
    }
    try {
        if (-not $Context.Roster -or $now -ge $Context.NextRoster) {
            $Context.Roster = Get-ChatRoster $Context
            Publish-ChatRoster $Root $Context $Context.Roster
            $Context.NextRoster = $now.AddSeconds((Get-Random -Minimum 20 -Maximum 31))
        }
        Receive-ChatDelivery $Root $Context
        Send-ChatOutbox $Root $Context $Context.Roster
    } catch {
        if ($_.Exception.Data['Status'] -in @(403,409)) { $Context.NextState = [datetime]::MinValue }
        throw
    }
}

function Show-AgentChatPairing($Root,$EndpointPath) {
    Add-Type -AssemblyName System.Windows.Forms
    $ep = Get-ChatEndpoint $EndpointPath
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($chatUtf8.GetBytes("$env:COMPUTERNAME-$env:USERNAME"))
        $client = [BitConverter]::ToString($hash,0,4).Replace('-','')
        $key = Get-ChatSecret $Root $ep.Device $client -Create
        $fingerprint = [BitConverter]::ToString($sha.ComputeHash($key),0,8)
        $hex = [BitConverter]::ToString($key).Replace('-','').ToLowerInvariant()
    } finally { $sha.Dispose() }
    $dialog = [Windows.Forms.Form]::new()
    try {
        $dialog.Text = 'Підключення офісного чату'
        $dialog.Width = 595
        $dialog.Height = 220
        $dialog.StartPosition = 'CenterScreen'
        $label = [Windows.Forms.Label]::new()
        $label.SetBounds(14,12,550,66)
        $label.Text = "ПК $client   Контролер $($ep.Device)   $($ep.Base)" + [Environment]::NewLine +
            "Відбиток ключа: $fingerprint" + [Environment]::NewLine + 'Скопіюйте ключ у форму контролера та підтвердьте підключення.'
        $field = [Windows.Forms.TextBox]::new()
        $field.SetBounds(14,84,545,24)
        $field.ReadOnly = $true
        $field.Text = $hex
        $open = [Windows.Forms.Button]::new()
        $open.Text = 'Відкрити форму контролера'
        $open.SetBounds(14,122,255,32)
        $open.Add_Click({ Start-Process -FilePath ($ep.Base + '/chat/pair?client=' + $client) })
        $dialog.Controls.AddRange(@($label,$field,$open))
        [void]$dialog.ShowDialog()
    } finally { $dialog.Dispose() }
}

function Start-AgentChatSession($Directory,$EndpointPath,$Client,$Root) {
    Initialize-ChatMailbox $Root
    $lockPath = Join-Path $Root 'worker.lock'
    Assert-AgentPlainPath $lockPath
    $lock = [IO.File]::Open($lockPath,'OpenOrCreate','ReadWrite','None')
    $session = @{ Lock=$lock; Stop=[Threading.ManualResetEvent]::new($false); Jobs=@() }
    try {
        Write-ChatObject (Join-Path $Root 'roster.json') @{self_id=$Client;agents=@()}
        foreach ($role in 'transport','tray') {
            $space = [RunspaceFactory]::CreateRunspace()
            $space.ApartmentState = if ($role -eq 'tray') { 'STA' } else { 'MTA' }
            $space.ThreadOptions = 'ReuseThread'
            $ps = [PowerShell]::Create()
            $job = @{ Role=$role; Shell=$ps; Space=$space; Task=$null }
            $session.Jobs += $job
            $space.Open()
            $ps.Runspace = $space
            [void]$ps.AddScript({
                param($role,$directory,$endpoint,$client,$root,$stop)
                $ErrorActionPreference = 'Stop'
                . (Join-Path $directory 'agent_update.ps1')
                . (Join-Path $directory 'agent_chat_ipc.ps1')
                if ($role -eq 'tray') {
                    & (Join-Path $directory 'agent_chat_ui.ps1') -MailboxRoot $root -ChatStopEvent $stop
                    return
                }
                $ctx = @{Client=$client; Seq=0L; Stop=$stop}
                $delay = 5
                while (-not $stop.WaitOne(0)) {
                    try {
                        Invoke-ChatCycle $root $endpoint $ctx
                        $delay = 5
                    } catch {
                        if ($stop.WaitOne(0)) { break }
                        $offline = @(foreach ($peer in $ctx.Roster.agents) {
                            [ordered]@{id=$peer.id;name=$peer.name;online=$false}
                        })
                        Publish-ChatRoster $root $ctx ([ordered]@{self_id=$client;agents=$offline})
                        $ctx.NextRoster = [datetime]::MinValue
                        $delay = [Math]::Min(30,2*$delay)
                    }
                    [void]$stop.WaitOne(1000*$delay - (Get-Random -Minimum 0 -Maximum 901))
                }
            }).AddArgument($role).AddArgument($Directory).AddArgument($EndpointPath).AddArgument($Client).AddArgument($Root).AddArgument($session.Stop)
            $job.Task = $ps.BeginInvoke()
        }
        $session
    } catch { Stop-AgentChatSession $session; throw }
}

function Stop-AgentChatSession($Session) {
    if (-not $Session) { return }
    [void]$Session.Stop.Set()
    foreach ($job in $Session.Jobs) {
        if ($job.Task) {
            if (-not $job.Task.AsyncWaitHandle.WaitOne(6500)) { $job.Shell.Stop() }
            try { [void]$job.Shell.EndInvoke($job.Task) } catch { $job.Failure = $_.Exception }
        }
        $job.Shell.Dispose()
        $job.Space.Dispose()
    }
    $Session.Stop.Dispose()
    $Session.Lock.Dispose()
}
