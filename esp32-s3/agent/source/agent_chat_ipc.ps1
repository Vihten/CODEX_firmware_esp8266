# Windows PowerShell 5.1. Dot-source agent_update.ps1 for durable file operations.
. (Join-Path $PSScriptRoot 'agent_chat_peer.ps1')
$chatUtf8 = [Text.UTF8Encoding]::new($false, $true)
$chatUuid = '\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\z'

function Request-LocalAgentRestart {
    $name = $env:LED_FORGE_TRAY_RESTART
    if ($name -cnotmatch '\ALocal\\LEDForgeTrayRestart-[0-9a-f]{32}\z') { return $false }
    try {
        $event = [Threading.EventWaitHandle]::OpenExisting($name,[Security.AccessControl.EventWaitHandleRights]::Modify)
        try { return $event.Set() } finally { $event.Dispose() }
    } catch [Threading.WaitHandleCannotBeOpenedException] { return $false }
}

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
    foreach ($name in 'inbox','outbox','seen','attempts','failed','deliveries','board-attempts','trusted') {
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

function Invoke-ChatRest($Context,[ValidateSet('state','send')]$Operation,
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

function Sync-ChatTrust($Root,$Context) {
    $bindings = @{}
    foreach ($file in [IO.Directory]::EnumerateFiles((Join-Path $Root 'trusted'),'*.json')) {
        if ($bindings.Count -ge 64) { break }
        try {
            $id = [IO.Path]::GetFileNameWithoutExtension($file)
            $row = Read-ChatObject $file
            if ($id -cnotmatch '\A[0-9A-F]{8}\z' -or $id -ceq $Context.Client -or $row.id -cne $id -or
                $row.fingerprint -isnot [string] -or $row.fingerprint -cnotmatch '\A[0-9A-F]{64}\z' -or
                $row.name -isnot [string] -or [string]::IsNullOrWhiteSpace($row.name) -or
                $row.name -match '[\p{Cc}]' -or $row.name.Length -gt 128) { continue }
            $bindings[$id] = $row
        } catch { continue }
    }
    if (-not $Context.Applied) { $Context.Applied = @{} }
    foreach ($id in @($Context.Applied.Keys)) {
        $old = $Context.Applied[$id]
        $new = $bindings[$id]
        if (-not $new -or $old.fingerprint -cne $new.fingerprint -or $old.name -cne $new.name) {
            Remove-ChatPeerTrust $Context.Peer $id
            $Context.Applied.Remove($id)
        }
    }
    if ($Context.Peer) {
        foreach ($id in $bindings.Keys) {
            if (-not $Context.Applied.ContainsKey($id)) {
                $row = $bindings[$id]
                try { Set-ChatPeerTrust $Context.Peer $id $row.name $row.fingerprint } catch { continue }
                $Context.Applied[$id] = $row
            }
        }
    }
    $Context.Bindings = $bindings
}

function Publish-ChatRoster($Root,$Context,$EndpointPath) {
    $Context.Online = @{}
    $rows = @([ordered]@{id=$Context.Client;name=$Context.Name;online=$true})
    $candidates = @()
    if ($Context.Peer) {
        foreach ($peer in (Get-ChatPeerRoster $Context.Peer | Sort-Object id)) {
            $binding = $Context.Applied[$peer.id]
            if ($binding) {
                if ($peer.online) { $Context.Online[$peer.id] = $true }
                $rows += [ordered]@{id=$peer.id;name=$binding.name;online=[bool]$peer.online;trusted=$true;fingerprint=$binding.fingerprint}
            }
        }
        $candidates = @(foreach ($peer in (Get-ChatPeerCandidates $Context.Peer | Sort-Object id,pin,address)) {
            [ordered]@{id=$peer.id;name=$peer.name;fingerprint=$peer.pin;address=[string]$peer.address}
        })
    }
    # Board availability is a local hint. Credentials are opened only for an explicitly queued board send.
    $ep = $null
    try { $ep = Get-ChatEndpoint $EndpointPath } catch {}
    $Context.Endpoint = $ep
    $boardReady = $ep -and [IO.File]::Exists((Join-Path $Root "chat-key-$($ep.Device)-$($Context.Client).bin"))
    $rows += [ordered]@{id='board';name='Табло';online=[bool]$boardReady}
    $snapshot = [ordered]@{self_id=$Context.Client;self_fingerprint=$Context.Peer.Pin;agents=$rows;candidates=$candidates}
    $json = ConvertTo-Json -InputObject $snapshot -Depth 5 -Compress
    if ($json -cne $Context.LastSnapshot) {
        Write-AgentDurable (Join-Path $Root 'roster.json') ($chatUtf8.GetBytes($json))
        $Context.LastSnapshot = $json
    }
}

function Get-ChatBoardBoot($Root,$Context,$Target) {
    $ep = $Context.Endpoint
    if (-not $ep) { throw 'Board endpoint unavailable' }
    if ($Target.device -and $Target.device -cne $ep.Device) { throw 'Selected board unavailable' }
    if ($Context.Device -cne $ep.Device -or -not $Context.Key) {
        $Context.Key = Get-ChatSecret $Root $ep.Device $Context.Client
        $Context.Device = $ep.Device
        $Context.Seq = 0L
        $Context.SequenceLimit = 0L
    }
    $Context.Base = $ep.Base
    $state = Invoke-ChatRest $Context state
    if ($state.boot -isnot [string] -or $state.boot -cnotmatch '\A[0-9a-f]{16}\z') { throw 'Invalid board boot' }
    $Context.Boot = $state.boot
}

function Get-ChatDraftDigest($Message) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        $value = ($Message.recipient_ids -join ',') + [char]10 + $Message.text
        [BitConverter]::ToString($hash.ComputeHash($chatUtf8.GetBytes($value)))
    } finally { $hash.Dispose() }
}

function New-ChatDelivery($Context,$Message,$Legacy) {
    $unique = @{}
    $targets = @(foreach ($id in $Message.recipient_ids) {
        if ($id -isnot [string] -or $id -cnotmatch '\A([0-9A-F]{8}|board)\z' -or $id -ceq $Context.Client -or $unique.ContainsKey($id)) {
            throw 'Invalid recipient'
        }
        $unique[$id] = $true
        $pin = if ($id -cne 'board' -and $Context.Bindings.ContainsKey($id)) { $Context.Bindings[$id].fingerprint }
        $device = if ($id -ceq 'board') { $Context.Endpoint.Device }
        $state = if ($Legacy) { 'uncertain' } elseif ($id -cne 'board' -and -not $pin) { 'failed' } else { 'pending' }
        [pscustomobject]@{id=$id;state=$state;started=0L;retry=0L;pin=$pin;device=$device;boot=$null}
    })
    [pscustomobject]@{id=$Message.id;digest=(Get-ChatDraftDigest $Message);queued=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds();targets=$targets}
}

function Send-ChatTarget($Root,$Context,$Message,$Record,$Target,$Journal,$Now) {
    if ($Target.state -cne 'pending' -or ($Context.Stop -and $Context.Stop.WaitOne(0))) { return }
    if ($Target.started -and ($Now -lt $Target.started -or $Now - $Target.started -ge 590)) {
        $Target.state = 'uncertain'
    } elseif (-not $Target.started -and ($Now -lt $Record.queued -or $Now - $Record.queued -ge 86400)) {
        $Target.state = 'failed'
    } elseif ($Now -lt $Target.retry) { return }
    else {
        $board = $Target.id -ceq 'board'
        if (-not $board -and $Context.Bindings[$Target.id].fingerprint -cne $Target.pin) {
            $Target.state = if ($Target.started) { 'uncertain' } else { 'failed' }
        } else {
            try {
                if ($board) {
                    Get-ChatBoardBoot $Root $Context $Target
                    if ($Target.boot -and $Target.boot -cne $Context.Boot) { $Target.state = 'uncertain' }
                    else { $Target.boot = $Context.Boot; $Target.device = $Context.Device }
                } elseif (-not $Context.Peer -or -not $Context.Online.ContainsKey($Target.id)) { return }
            } catch {
                $Target.retry = $Now + 10
                Write-ChatObject $Journal $Record
                return
            }
            if ($Target.state -ceq 'pending') {
                if (-not $Target.started) { $Target.started = $Now }
                $Target.retry = $Now + 10
                # Persist the exact UUID, board boot or peer pin before the first uncertain network write.
                Write-ChatObject $Journal $Record
                try {
                    if ($board) {
                        $reply = Invoke-ChatRest $Context send $Message.id 'board' $Message.text
                        if ($reply.ok -isnot [bool] -or -not $reply.ok) { throw 'Invalid board receipt' }
                    } else {
                        $receipt = Send-ChatPeerMessage $Context.Peer $Target.id $Message.id $Message.text
                        if ($receipt -isnot [string] -or $receipt -cne $Message.id) { throw 'Invalid peer receipt' }
                    }
                    $Target.state = 'delivered'
                } catch { return }
            }
        }
    }
    Write-ChatObject $Journal $Record
}

function Send-ChatBoardDraft($Root,$Context,$Message,$Record) {
    if ($Record.digest -cne (Get-ChatDraftDigest $Message)) { return }
    $board = $Record.targets | Where-Object id -ceq 'board'
    if ($board.state -cne 'pending') { return }
    $path = Join-Path $Root "board-attempts/$($Message.id).json"
    if ([IO.File]::Exists($path)) {
        $progress = Read-ChatObject $path
        if ($progress.id -cne $Record.id -or $progress.digest -cne $Record.digest) { return }
    } else {
        $progress = [pscustomobject]@{id=$Record.id;digest=$Record.digest;queued=$Record.queued;targets=@($board)}
    }
    Send-ChatTarget $Root $Context $Message $progress $progress.targets[0] $path ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
}

function Invoke-ChatBoardCycle($Root,$EndpointPath,$Context) {
    foreach ($file in [IO.Directory]::EnumerateFiles((Join-Path $Root 'outbox'),'*.json')) {
        if ($Context.Stop.WaitOne(0)) { return }
        try {
            $msg = Read-ChatObject $file
            if ($msg.id -cnotmatch $chatUuid -or $msg.id -cne [IO.Path]::GetFileNameWithoutExtension($file) -or
                $msg.recipient_ids -cnotcontains 'board') { continue }
            $record = Read-ChatObject (Join-Path $Root "deliveries/$($msg.id).json")
            $Context.Endpoint = $null
            try { $Context.Endpoint = Get-ChatEndpoint $EndpointPath } catch {}
            Send-ChatBoardDraft $Root $Context $msg $record
        } catch { continue }
    }
}

function Send-ChatOutbox($Root,$Context) {
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    foreach ($path in [IO.Directory]::EnumerateFiles((Join-Path $Root 'outbox'),'*.json')) {
        if ($Context.Stop -and $Context.Stop.WaitOne(0)) { return }
        $name = [IO.Path]::GetFileName($path)
        $id = [IO.Path]::GetFileNameWithoutExtension($path)
        $failed = Join-Path $Root "failed/$name"
        $journal = Join-Path $Root "deliveries/$name"
        try {
            $msg = Read-ChatObject $path
            if ($id -cnotmatch $chatUuid -or $msg.id -cne $id -or -not (Test-ChatText $msg.text) -or
                $msg.recipient_ids -isnot [array] -or $msg.recipient_ids.Count -lt 1 -or $msg.recipient_ids.Count -gt 13) { throw 'Invalid draft' }
            if ([IO.File]::Exists($journal)) {
                $record = Read-ChatObject $journal
                if ($record.id -cne $id -or $record.digest -cne (Get-ChatDraftDigest $msg) -or
                    $record.targets -isnot [array] -or ($record.targets.id -join ',') -cne ($msg.recipient_ids -join ',')) { throw 'Draft or journal changed' }
            } else {
                $legacy = [IO.File]::Exists((Join-Path $Root "attempts/$name"))
                $record = New-ChatDelivery $Context $msg $legacy
                Write-ChatObject $journal $record
            }
        } catch {
            if (-not [IO.File]::Exists($failed)) { [IO.File]::Move($path,$failed) }
            continue
        }
        foreach ($t in $record.targets | Where-Object id -cne 'board') {
            Send-ChatTarget $Root $Context $msg $record $t $journal $now
        }
        $board = $record.targets | Where-Object id -ceq 'board'
        if ($board.state -ceq 'pending') {
            if (-not $Context.SeparateBoard) { Send-ChatBoardDraft $Root $Context $msg $record }
            $progressPath = Join-Path $Root "board-attempts/$id.json"
            if ([IO.File]::Exists($progressPath)) {
                $progress = Read-ChatObject $progressPath
                if ($progress.id -ceq $id -and $progress.digest -ceq $record.digest -and
                    $progress.targets[0].state -cin @('delivered','uncertain','failed')) {
                    $board.state = $progress.targets[0].state
                    Write-ChatObject $journal $record
                }
            }
        }
        if ($record.targets.state -ccontains 'pending') { continue }
        if (@($record.targets | Where-Object state -cne 'delivered').Count) {
            if (-not [IO.File]::Exists($failed)) { [IO.File]::Move($path,$failed) }
        } else { [IO.File]::Delete($path) }
    }
}

function Invoke-ChatCycle($Root,$EndpointPath,$Context) {
    $Context.Root = $Root
    Sync-ChatTrust $Root $Context
    Publish-ChatRoster $Root $Context $EndpointPath
    Send-ChatOutbox $Root $Context
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
        $dialog.Text = 'Підключення табло'
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

function Start-AgentChatSession($Directory,$EndpointPath,$Client,$Root,$Name=$env:COMPUTERNAME,[hashtable]$PeerOptions=@{}) {
    Initialize-ChatMailbox $Root
    $lockPath = Join-Path $Root 'worker.lock'
    Assert-AgentPlainPath $lockPath
    $lock = [IO.File]::Open($lockPath,'OpenOrCreate','ReadWrite','None')
    $session = @{ Lock=$lock; Stop=[Threading.ManualResetEvent]::new($false); Jobs=@() }
    try {
        Write-ChatObject (Join-Path $Root 'roster.json') @{self_id=$Client;agents=@()}
        foreach ($role in 'transport','board','tray') {
            $space = [RunspaceFactory]::CreateRunspace()
            $space.ApartmentState = if ($role -eq 'tray') { 'STA' } else { 'MTA' }
            $space.ThreadOptions = 'ReuseThread'
            $ps = [PowerShell]::Create()
            $job = @{ Role=$role; Shell=$ps; Space=$space; Task=$null }
            $session.Jobs += $job
            $space.Open()
            $ps.Runspace = $space
            [void]$ps.AddScript({
                param($role,$directory,$endpoint,$client,$root,$stop,$name,$peerOptions)
                $ErrorActionPreference = 'Stop'
                . (Join-Path $directory 'agent_update.ps1')
                . (Join-Path $directory 'agent_chat_ipc.ps1')
                if ($role -eq 'tray') {
                    & (Join-Path $directory 'agent_chat_ui.ps1') -MailboxRoot $root -ChatStopEvent $stop
                    return
                }
                $ctx = @{Client=$client; Name=$name; Seq=0L; Stop=$stop; Root=$root; SeparateBoard=$true}
                if ($role -eq 'board') {
                    while (-not $stop.WaitOne(0)) {
                        Invoke-ChatBoardCycle $root $endpoint $ctx
                        [void]$stop.WaitOne(1500)
                    }
                    return
                }
                $nextStart = [datetime]::MinValue
                try {
                    while (-not $stop.WaitOne(0)) {
                        if (-not $ctx.Peer -and [datetime]::UtcNow -ge $nextStart) {
                            $nextStart = [datetime]::UtcNow.AddSeconds(15)
                            try { $ctx.Peer = Start-ChatPeerRuntime -Root $root -Client $client -Name $name @peerOptions } catch {}
                        }
                        try { Invoke-ChatCycle $root $endpoint $ctx } catch {
                            if ($stop.WaitOne(1000)) { break }
                        }
                        [void]$stop.WaitOne(1500)
                    }
                } finally { if ($ctx.Peer) { Stop-ChatPeerRuntime $ctx.Peer } }
            }).AddArgument($role).AddArgument($Directory).AddArgument($EndpointPath).AddArgument($Client).AddArgument($Root).AddArgument($session.Stop).AddArgument($Name).AddArgument($PeerOptions)
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
