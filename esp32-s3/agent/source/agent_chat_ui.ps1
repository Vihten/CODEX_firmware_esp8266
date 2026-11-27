#requires -Version 5.1
[CmdletBinding()]
param([string]$MailboxRoot = (Join-Path $env:LOCALAPPDATA 'CodexLanAgent\chat'), [Threading.WaitHandle]$ChatStopEvent)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
if (-not ('OfficeChatToast' -as [type])) {
    Add-Type -WarningAction SilentlyContinue -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition @"
using System.Windows.Forms;
public sealed class OfficeChatToast : Form {
    protected override bool ShowWithoutActivation { get { return true; } }
    protected override CreateParams CreateParams {
        get { var cp = base.CreateParams; cp.ExStyle |= 0x08000080; return cp; }
    }
    protected override void WndProc(ref Message m) {
        if (m.Msg == 0x21) { m.Result = (System.IntPtr)3; return; }
        base.WndProc(ref m);
    }
}
"@
}

# Columns 2 and 5 are aisles. Empty desks occupy the upper left pair.
$seats = @(
    @{Name='Влад'; X=0; Y=1; Aliases=@('Vlad','Vladyslav','Vladislav','Владислав')}
    @{Name='Олексій'; X=1; Y=1; Aliases=@('Oleksii','Oleksiy','Oleksij','Alexey','Aleksey','Alex','Aleksei','Alexei','Алексей')}
    @{Name='Нестор'; X=3; Y=0; Aliases=@('Nestor')}
    @{Name='Дімасік'; X=4; Y=0; Aliases=@('Dimasik','Dimas','Dima','Димасик','Дімасик')}
    @{Name='Саша'; X=3; Y=1; Aliases=@('Sasha','Sashko','Oleksandr','Alexander','Александр','Олександр')}
    @{Name='Микола'; X=4; Y=1; Aliases=@('Mykola','Mikola','Kolya','Коля','Nikolay','Nikolai','Николай')}
    @{Name='Олег'; X=6; Y=0; Aliases=@('Oleg','Oleh')}
)

function Read-ChatJson($Path) {
    [IO.File]::ReadAllText($Path,[Text.Encoding]::UTF8) | ConvertFrom-Json -ErrorAction Stop
}

function Initialize-Mailbox {
    foreach ($folder in 'inbox','outbox','seen') {
        [void][IO.Directory]::CreateDirectory((Join-Path $MailboxRoot $folder))
    }
}

function Update-Send {
    $selected = @($buttons.Values | Where-Object { $_.Checked -and $_.Enabled })
    $body = $draft.Text.Replace([Environment]::NewLine,"`n")
    $bytes = [Text.Encoding]::UTF8.GetByteCount($body)
    $counter.Text = "$bytes / 512 Б"
    $valid = $bytes -le 512 -and $body -notmatch '[\x00-\x09\x0b-\x1f\x7f]'
    $write.Enabled = $selected.Count -gt 0
    $send.Enabled = $selected.Count -gt 0 -and $valid -and -not [string]::IsNullOrWhiteSpace($body)
}

function Get-TrustRoster {
    try { Read-ChatJson (Join-Path $MailboxRoot 'roster.json') } catch { return $null }
}

function Test-PeerFingerprint($value) {
    $value -is [string] -and $value -cmatch '\A[0-9A-F]{64}\z'
}

function Test-PeerIdentity($peer) {
    $peer.id -is [string] -and $peer.id -cmatch '\A[0-9A-F]{8}\z' -and
        (Test-PeerFingerprint $peer.fingerprint) -and $peer.name -is [string] -and
        [Text.Encoding]::UTF8.GetByteCount($peer.name) -le 48 -and -not [string]::IsNullOrWhiteSpace($peer.name) -and
        $peer.name -notmatch '[\p{Cc}\p{Cf}\p{Zl}\p{Zp}]'
}

function Get-TrustBindingPath($id) {
    if ($id -isnot [string] -or $id -cnotmatch '\A[0-9A-F]{8}\z') { throw 'Invalid peer ID' }
    Join-Path $MailboxRoot "trusted\$id.json"
}

function Test-BindingMatches($peer) {
    if (-not (Test-PeerIdentity $peer)) { return $false }
    try {
        $saved = Read-ChatJson (Get-TrustBindingPath $peer.id)
        return (Test-PeerIdentity $saved) -and $saved.id -ceq $peer.id -and
            $saved.fingerprint -ceq $peer.fingerprint -and $saved.name -ceq $peer.name
    } catch { return $false }
}

function Test-CandidateConflict($snapshot,$peer) {
    foreach ($candidate in $snapshot.candidates) {
        if ($candidate.id -ceq $peer.id -and $candidate.fingerprint -cne $peer.fingerprint) { return $true }
    }
    return $false
}

function Test-CanApprovePeer($snapshot,$peer) {
    if (-not (Test-PeerIdentity $peer) -or $snapshot.self_id -cnotmatch '\A[0-9A-F]{8}\z' -or
        -not (Test-PeerFingerprint $snapshot.self_fingerprint) -or $peer.id -ceq $snapshot.self_id -or
        $peer.fingerprint -ceq $snapshot.self_fingerprint -or (Test-CandidateConflict $snapshot $peer)) { return $false }
    $matches = @($snapshot.candidates | Where-Object { $_.id -ceq $peer.id })
    if ($matches.Count -ne 1 -or $matches[0].fingerprint -cne $peer.fingerprint -or
        $matches[0].name -cne $peer.name) { return $false }
    return -not (Test-Path -LiteralPath (Get-TrustBindingPath $peer.id) -ErrorAction Stop)
}

function Save-VerifiedPeer($peer,$displayedRoster) {
    $snapshot = Get-TrustRoster
    if ($displayedRoster.self_id -cne $snapshot.self_id -or
        $displayedRoster.self_fingerprint -cne $snapshot.self_fingerprint -or
        -not (Test-CanApprovePeer $snapshot $peer)) { throw 'Identity changed' }
    $path = Get-TrustBindingPath $peer.id
    $directory = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    $temporary = Join-Path $directory.FullName ([guid]::NewGuid().ToString() + '.tmp')
    try {
        $binding = [ordered]@{id=$peer.id;name=$peer.name;fingerprint=$peer.fingerprint}
        [IO.File]::WriteAllText($temporary,($binding | ConvertTo-Json -Compress))
        [IO.File]::Move($temporary,$path)
    } finally { if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) } }
}

function Format-PeerFingerprint($value) {
    if (Test-PeerFingerprint $value) { return ($value -replace '(.{4})(?=.)','$1 ') }
    return ''
}

function Get-PeerComparisonCode($localPin,$remotePin) {
    if (-not (Test-PeerFingerprint $localPin) -or -not (Test-PeerFingerprint $remotePin) -or
        $localPin -ceq $remotePin) { return '' }
    $pins = [string[]]@($localPin,$remotePin)
    [Array]::Sort($pins,[StringComparer]::Ordinal)
    $inputBytes = [Text.Encoding]::ASCII.GetBytes(('LFCHAT-PAIR-V1',$pins[0],$pins[1] -join [char]10))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hex = [BitConverter]::ToString($sha.ComputeHash($inputBytes),0,6).Replace('-','') }
    finally { $sha.Dispose() }
    return ($hex -replace '(.{4})(?=.)','$1 ')
}

function Copy-ChatIdentity([string]$value) {
    [Windows.Forms.Clipboard]::SetText($value)
}

function Update-PeerVerification {
    $peer = $peerList.SelectedItem.Identity
    $code = ''
    $eligible = $false
    if (-not $peer) {
        $message = if ($peerList.Items.Count) { "Знайдено ПК: $($peerList.Items.Count). Виберіть співрозмовника зі списку." }
            else { 'Шукаємо інші ПК у локальній мережі…' }
    } elseif (Test-CandidateConflict $trustSnapshot $peer) {
        $message = 'Конфлікт ключів для цього ID. Звірте дані з власником ПК.'
    } elseif (Test-BindingMatches $peer) {
        $message = 'Цей ПК уже підтверджено. Інший власник може звірити цей код.'
        $code = Get-PeerComparisonCode $trustSnapshot.self_fingerprint $peer.fingerprint
    } elseif (Test-CanApprovePeer $trustSnapshot $peer) {
        $message = 'Підтвердьте збіг на кожному ПК лише після звірки всіх трьох груп.'
        $code = Get-PeerComparisonCode $trustSnapshot.self_fingerprint $peer.fingerprint
        $eligible = $code.Length -gt 0
    } elseif (Test-Path -LiteralPath (Get-TrustBindingPath $peer.id)) {
        $message = "Збережена прив’язка відрізняється. Заміну заблоковано."
    } else {
        $message = 'Перевірка недоступна. Оновіть список ПК.'
    }
    $pairCode.Text = $code
    $trustStatus.Text = $message
    $codesMatch.Enabled = $eligible
    $approvePeer.Enabled = $eligible -and $codesMatch.Checked
}

function Refresh-PeerTrust([switch]$Automatic) {
    $previous = $peerList.SelectedItem
    $snapshot = Get-TrustRoster
    $sameLocal = $snapshot.self_id -ceq $trustSnapshot.self_id -and
        $snapshot.self_fingerprint -ceq $trustSnapshot.self_fingerprint
    $keepCheck = $Automatic -and $sameLocal -and $codesMatch.Checked -and $codesMatch.Enabled
    $script:trustSnapshot = $snapshot
    $copyIdentity.Enabled = (Test-PeerFingerprint $snapshot.self_fingerprint) -and $snapshot.self_id -cmatch '\A[0-9A-F]{8}\z'
    $ownFingerprint.Text = if ($copyIdentity.Enabled) { Format-PeerFingerprint $snapshot.self_fingerprint }
        else { 'Очікуємо локальний ключ.' }
    $ownLabel.Text = "Відбиток цього ПК (ID $($snapshot.self_id))"

    $listed = @{}
    $items = @(foreach ($peer in (@($snapshot.candidates) + @($snapshot.agents))) {
        if (-not (Test-PeerIdentity $peer) -or $peer.id -ceq $snapshot.self_id) { continue }
        $key = "$($peer.id)/$($peer.fingerprint)"
        if ($listed.ContainsKey($key)) { continue }
        $listed[$key] = $true
        [pscustomobject]@{Label="$($peer.name) ($($peer.id))";Identity=$peer;Key="$key/$($peer.name)"}
    })
    $changed = $items.Count -ne $peerList.Items.Count
    $index = -1
    for ($i = 0; $i -lt $items.Count; $i++) {
        if (-not $changed -and $items[$i].Key -cne $peerList.Items[$i].Key) { $changed = $true }
        if ($previous -and $items[$i].Key -ceq $previous.Key -and $sameLocal) { $index = $i }
    }
    if ($index -ge 0) {
        $eligible = Test-CanApprovePeer $snapshot $items[$index].Identity
        $keepCheck = $keepCheck -and $eligible
        if ($Automatic -and -not $eligible -and
            ((Test-CandidateConflict $snapshot $items[$index].Identity) -or -not (Test-BindingMatches $items[$index].Identity))) {
            $index = -1
        }
    } else { $keepCheck = $false }
    if (-not $Automatic -and $index -lt 0 -and $items.Count) { $index = 0 }

    # Only rebuild a changed list. An idle tick must leave an open dropdown and keyboard focus alone.
    $script:refreshingPeerList = $true
    try {
        if ($changed) {
            $peerList.BeginUpdate()
            try {
                $peerList.Items.Clear()
                foreach ($item in $items) { [void]$peerList.Items.Add($item) }
            } finally { $peerList.EndUpdate() }
        }
        $peerList.SelectedIndex = $index
        $codesMatch.Checked = $keepCheck
    } finally { $script:refreshingPeerList = $false }
    Update-PeerVerification
}

function New-FingerprintField($readOnly,$label) {
    $field = New-Object Windows.Forms.TextBox
    $field.Dock = 'Fill'
    $field.ReadOnly = $readOnly
    $field.AccessibleName = $label
    $field.Font = New-Object Drawing.Font('Consolas',10)
    return $field
}

function Show-PeerTrust {
    if ($trustWindow -and -not $trustWindow.IsDisposed) { $trustWindow.Activate(); return }
    $script:trustWindow = New-Object Windows.Forms.Form
    $trustWindow.Text = "Перевірити комп’ютери"
    $trustWindow.Font = $form.Font
    $trustWindow.AutoScaleMode = 'Dpi'
    $trustWindow.ClientSize = New-Object Drawing.Size(660,364)
    $trustWindow.FormBorderStyle = 'FixedDialog'
    $trustWindow.MaximizeBox = $false
    $trustWindow.StartPosition = 'CenterScreen'
    $trustWindow.Padding = 16
    $layout = New-ChatGrid
    foreach ($height in 36,34,28,38,28,40,36,40,42) {
        [void]$layout.RowStyles.Add([Windows.Forms.RowStyle]::new('Absolute',$height))
    }
    $trustWindow.Controls.Add($layout)
    $script:ownLabel = New-ChatLabel ''
    $identityRow = New-ChatGrid
    [void]$identityRow.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Percent',100))
    [void]$identityRow.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Absolute',240))
    $identityRow.Controls.Add($ownLabel,0,0)
    $script:copyIdentity = New-Object Windows.Forms.Button
    $copyIdentity.Dock = 'Fill'
    $copyIdentity.Text = 'Скопіювати ID та відбиток'
    $copyIdentity.Add_Click({
        try {
            Copy-ChatIdentity ("ID " + $trustSnapshot.self_id + "  " + $ownFingerprint.Text)
            $trustStatus.Text = 'ID та відбиток цього ПК скопійовано.'
        } catch { $trustStatus.Text = 'Буфер обміну зайнято. Виділіть відбиток і скопіюйте його вручну.' }
    })
    $identityRow.Controls.Add($copyIdentity,1,0)
    $layout.Controls.Add($identityRow,0,0)
    $script:ownFingerprint = New-FingerprintField $true 'Відбиток цього ПК'
    $layout.Controls.Add($ownFingerprint,0,1)
    $layout.Controls.Add((New-ChatLabel 'Інший ПК'),0,2)
    $script:peerList = New-Object Windows.Forms.ComboBox
    $peerList.AccessibleName = 'Інший ПК'
    $peerList.DropDownStyle = 'DropDownList'
    $peerList.DisplayMember = 'Label'
    $peerList.Dock = 'Fill'
    $layout.Controls.Add($peerList,0,3)
    $layout.Controls.Add((New-ChatLabel 'Виберіть один одного на обох ПК. Звірте цей код особисто або телефоном.'),0,4)
    $script:pairCode = New-ChatLabel ''
    $pairCode.Font = New-Object Drawing.Font('Consolas',18,[Drawing.FontStyle]::Bold)
    $pairCode.AccessibleName = 'Спільний код перевірки'
    $layout.Controls.Add($pairCode,0,5)
    $script:codesMatch = New-Object Windows.Forms.CheckBox
    $codesMatch.Dock = 'Fill'
    $codesMatch.Text = 'Я звірив усі три групи коду з власником іншого ПК'
    $layout.Controls.Add($codesMatch,0,6)
    $script:trustStatus = New-ChatLabel ''
    $layout.Controls.Add($trustStatus,0,7)
    $actions = New-Object Windows.Forms.FlowLayoutPanel
    $actions.Dock = 'Fill'
    $actions.FlowDirection = 'RightToLeft'
    $layout.Controls.Add($actions,0,8)
    $script:approvePeer = New-Object Windows.Forms.Button
    $approvePeer.Text = 'Підтвердити ПК'
    $approvePeer.Size = New-Object Drawing.Size(150,30)
    $approvePeer.Add_Click({
        $peer = $peerList.SelectedItem.Identity
        try {
            if (-not $codesMatch.Checked -or -not $codesMatch.Enabled -or -not $pairCode.Text) { return }
            Save-VerifiedPeer $peer $trustSnapshot
            $codesMatch.Checked = $false
            Update-Seats
            Update-PeerVerification
        } catch {
            Refresh-PeerTrust
            $trustStatus.Text = 'Підтвердження не збережено. Перевірте дані ще раз.'
        }
    })
    $actions.Controls.Add($approvePeer)
    $refresh = New-Object Windows.Forms.Button
    $refresh.Text = 'Оновити список'
    $refresh.Size = New-Object Drawing.Size(150,30)
    $refresh.Add_Click({ Refresh-PeerTrust })
    $actions.Controls.Add($refresh)
    $peerList.Add_SelectedIndexChanged({ if (-not $refreshingPeerList) { $codesMatch.Checked = $false; Update-PeerVerification } })
    $codesMatch.Add_CheckedChanged({ if (-not $refreshingPeerList) { Update-PeerVerification } })
    Refresh-PeerTrust
    $timerForDialog = New-Object Windows.Forms.Timer
    $timerForDialog.Interval = 1500
    $timerForDialog.Add_Tick({ Refresh-PeerTrust -Automatic })
    $trustWindow.Tag = $timerForDialog
    $trustWindow.Add_Disposed({ param($window,$eventArgs) $window.Tag.Dispose() })
    $trustWindow.Show()
    $timerForDialog.Start()
}

function Update-Seats {
    $snapshot = Get-TrustRoster
    $rows = @($snapshot.agents)
    if ($snapshot.self_id -and $snapshot.self_name -and -not ($rows | Where-Object { $_.id -eq $snapshot.self_id })) {
        $rows += [pscustomobject]@{id=$snapshot.self_id;name=$snapshot.self_name;online=$true}
    }
    foreach ($seat in $seats) {
        $button = $buttons[$seat.Name]
        $names = @($seat.Name) + $seat.Aliases
        $peers = @($rows | Where-Object { $_.id -and $_.id -ne 'board' -and $names -contains ([string]$_.name).Trim() })
        $id = $null
        $fingerprint = $null
        $state = 'Поза мережею'
        $enabled = $false
        if (-not $peers.Count -and ($snapshot.candidates | Where-Object { $names -contains $_.name })) {
            $state = 'Не перевірено'
        }
        if ($peers.Count -gt 1) { $state = "Неоднозначне ім’я" }
        elseif ($peers.Count -eq 1) {
            $id = [string]$peers[0].id
            if ($id -eq $snapshot.self_id) { $state = 'Ви' }
            elseif ((Test-CandidateConflict $snapshot $peers[0])) { $state = 'Ключ змінено' }
            elseif ($peers[0].trusted -isnot [bool] -or -not $peers[0].trusted -or
                    -not (Test-BindingMatches $peers[0]) -or
                    @($rows | Where-Object { $_.id -ceq $id }).Count -ne 1) { $state = 'Не перевірено' }
            elseif ($peers[0].online -is [bool] -and $peers[0].online) {
                $state = 'У мережі'
                $fingerprint = $peers[0].fingerprint
                $enabled = $snapshot.self_id -cmatch '\A[0-9A-F]{8}\z' -and (Test-PeerFingerprint $snapshot.self_fingerprint)
            }
        }
        if ($id -and $recoveredPath -and $lastDelivery -and $lastDelivery.states[$id] -ceq 'delivered') {
            $state = 'Доставлено'
            $enabled = $false
        }
        if (-not $enabled -or $id -ne $button.Tag -or $seatFingerprints[$seat.Name] -cne $fingerprint) { $button.Checked = $false }
        $seatFingerprints[$seat.Name] = $fingerprint
        $button.Tag = $id
        $button.Enabled = $enabled
        $button.Text = $seat.Name + [Environment]::NewLine + $state
    }
    $boardRows = @($snapshot.agents | Where-Object { $_.id -ceq 'board' })
    $board.Enabled = $snapshot.self_id -and $boardRows.Count -eq 1 -and $boardRows[0].online -is [bool] -and $boardRows[0].online
    if ($recoveredPath -and $lastDelivery -and $lastDelivery.states['board'] -ceq 'delivered') { $board.Enabled = $false }
    if (-not $board.Enabled) { $board.Checked = $false }
    if (-not $snapshot.self_id) { $presence.Text = 'Очікуємо список учасників' }
    else { $presence.Text = 'Оберіть отримувачів' }
    Update-Send
}

function Set-DeliveryView($id,$ids,[bool]$recovering) {
    $names = @{}
    foreach ($target in $ids) {
        $names[$target] = $target
        if ($target -ceq 'board') { $names[$target] = 'Табло'; continue }
        foreach ($seat in $seats) {
            if ($buttons[$seat.Name].Tag -ceq $target) { $names[$target] = $seat.Name; break }
        }
        if ($lastDelivery -and $lastDelivery.id -ceq $id -and $lastDelivery.names.ContainsKey($target)) {
            $names[$target] = $lastDelivery.names[$target]
        }
    }
    $script:lastDelivery = @{id=$id;ids=@($ids);names=$names;states=@{};recovering=$recovering}
    $deliveryLink.Visible = $true
    Update-DeliveryView
}

function Update-DeliveryView {
    if (-not $lastDelivery) { return }
    $labels = @{pending='очікує';delivered='доставлено';failed='не доставлено';uncertain='доставку не підтверджено'}
    try {
        $record = Read-ChatJson (Join-Path $MailboxRoot "deliveries\$($lastDelivery.id).json")
        if ($record.id -ceq $lastDelivery.id -and $record.targets -is [array] -and
            ($record.targets.id -join ',') -ceq ($lastDelivery.ids -join ',')) {
            foreach ($target in $record.targets) {
                if ($target.state -cin @('pending','delivered','failed','uncertain')) {
                    $lastDelivery.states[$target.id] = $target.state
                }
            }
        }
    } catch { }
    $lines = foreach ($id in $lastDelivery.ids) {
        $description = if ($lastDelivery.states.ContainsKey($id)) { $labels[$lastDelivery.states[$id]] }
            elseif ($lastDelivery.recovering) { 'доставку не підтверджено' } else { 'очікує' }
        "$($lastDelivery.names[$id]) — $description"
    }
    $content = $lines -join [Environment]::NewLine
    if ($deliveryText.Text -cne $content) { $deliveryText.Text = $content }
}

function Show-DeliveryResults {
    if (-not $lastDelivery) { return }
    Set-ComposerMode $true
    [void]$deliveryText.Focus()
}

function Submit-Draft {
    $chosen = @($buttons.Values | Where-Object Checked | ForEach-Object Tag | Sort-Object -Unique)
    Update-Seats
    $ids = @($buttons.Values | Where-Object { $_.Enabled -and $_.Checked } | ForEach-Object Tag | Sort-Object -Unique)
    if (-not $send.Enabled -or $ids.Count -ne $chosen.Count) {
        $status.Text = 'Перевірте вибір отримувачів.'
        return
    }
    $id = [guid]::NewGuid().ToString()
    $path = Join-Path $MailboxRoot "outbox\$id.json"
    try {
        $count = @([IO.Directory]::EnumerateFiles((Join-Path $MailboxRoot 'outbox'),'*.json') | Select-Object -First 32).Count
        if ($count -ge 32) { $status.Text = 'Чергу заповнено. Спробуйте пізніше.'; return }
        $json = @{id=$id;recipient_ids=$ids;text=$draft.Text.Replace([Environment]::NewLine,"`n")} | ConvertTo-Json -Compress
        [IO.File]::WriteAllText("$path.tmp",$json,(New-Object Text.UTF8Encoding($false)))
        [IO.File]::Move("$path.tmp",$path)
        if ($script:recoveredPath) {
            [IO.File]::Delete($script:recoveredPath)
            $script:recoveredPath = $null
        }
        Set-DeliveryView $id $ids $false
        Set-ComposerMode $true
        $draft.Clear()
        $status.Text = "У черзі на надсилання ($($ids.Count))."
    } catch {
        $status.Text = 'Запис не вдався. Спробуйте ще раз.'
    } finally {
        if ([IO.File]::Exists("$path.tmp")) { [IO.File]::Delete("$path.tmp") }
    }
}

function Restore-ChatDraft {
    if ($draft.TextLength) {
        Open-ChatWindow
        Set-ComposerMode $true
        $status.Text = 'Спершу завершіть поточну чернетку.'
        return
    }
    $failed = Join-Path $MailboxRoot 'failed'
    if (-not [IO.Directory]::Exists($failed)) { return }
    $entry = Get-ChildItem -LiteralPath $failed -File -Filter '*.json' | Sort-Object LastWriteTimeUtc | Select-Object -First 1
    if (-not $entry) { return }
    $msg = Read-ChatJson $entry.FullName
    $uuid = [guid]::Empty
    if (-not [guid]::TryParseExact($entry.BaseName,'D',[ref]$uuid) -or $msg.id -cne $entry.BaseName -or
        $msg.recipient_ids -isnot [array] -or -not $msg.recipient_ids.Count) { return }
    foreach ($target in $msg.recipient_ids) {
        if ($target -isnot [string] -or $target -cnotmatch '\A([0-9A-F]{8}|board)\z') { return }
    }
    if ($msg.text -isnot [string]) { return }
    Open-ChatWindow
    foreach ($button in $buttons.Values) { $button.Checked = $false }
    $draft.Text = $msg.text.Replace([string][char]10,[Environment]::NewLine)
    $script:recoveredPath = $entry.FullName
    Set-DeliveryView $msg.id $msg.recipient_ids $true
    Update-Seats
    Set-ComposerMode $true
    $status.Text = 'Перевірте результати перед повторним надсиланням.'
}

function New-ChatLabel($caption) {
    $label = New-Object Windows.Forms.Label
    $label.Text = $caption
    $label.UseMnemonic = $false
    $label.Dock = 'Fill'
    $label.TextAlign = 'MiddleLeft'
    $label
}

function New-ChatGrid {
    $panel = New-Object Windows.Forms.TableLayoutPanel
    $panel.Dock = 'Fill'
    $panel.Margin = 0
    $panel
}

function Place-ChatWindow($window,$area) {
    $x = [Math]::Max($area.Left,$area.Right-$window.Width-12)
    $y = [Math]::Max($area.Top,$area.Bottom-$window.Height-12)
    $window.Location = New-Object Drawing.Point($x,$y)
}

function Set-ComposerMode([bool]$expanded) {
    $form.SuspendLayout()
    try {
        $caption.Visible = $expanded
        $draft.Visible = $expanded
        $send.Visible = $expanded
        $write.Visible = -not $expanded
        $main.RowStyles[3].Height = if ($expanded) { 36 } else { 0 }
        $area = if ($form.Visible) { [Windows.Forms.Screen]::FromControl($form).WorkingArea }
                else { [Windows.Forms.Screen]::FromPoint([Windows.Forms.Cursor]::Position).WorkingArea }
        $height = if ($expanded) { 452 } else { 266 }
        $showResults = $expanded -and $null -ne $lastDelivery
        $deliveryText.Visible = $showResults
        $main.RowStyles[6].Height = if ($showResults) { 74 } else { 0 }
        if ($showResults) { $height += 74 }
        $form.ClientSize = New-Object Drawing.Size(740,$height)
        Place-ChatWindow $form $area
    } finally { $form.ResumeLayout($true) }
    if ($expanded) { [void]$draft.Focus() }
}

function Open-ChatWindow {
    Update-Seats
    Set-ComposerMode $false
    $form.Show()
    $form.Activate()
}

function New-ChatWindow {
    $script:form = New-Object Windows.Forms.Form
    $form.Text = 'Офісний чат'
    $form.Font = New-Object Drawing.Font('Segoe UI',10)
    $form.AutoScaleMode = 'Dpi'
    $form.ClientSize = New-Object Drawing.Size(740,452)
    $form.FormBorderStyle = 'FixedSingle'
    $form.MaximizeBox = $false
    $form.StartPosition = 'Manual'
    $form.Padding = 16
    $script:main = New-ChatGrid
    $form.Controls.Add($main)
    foreach ($height in 32,28,132,36,-1,42,0) {
        if ($height -eq -1) { [void]$main.RowStyles.Add((New-Object Windows.Forms.RowStyle('Percent',100))) }
        else { [void]$main.RowStyles.Add((New-Object Windows.Forms.RowStyle('Absolute',$height))) }
    }
    $script:board = New-Object Windows.Forms.CheckBox
    $board.Appearance = 'Button'
    $board.Tag = 'board'
    $board.Text = 'Табло'
    $board.Add_CheckedChanged({ Update-Send })
    $board.Dock = 'None'
    $board.Anchor = 'None'
    $board.Size = New-Object Drawing.Size(176,30)
    $board.TextAlign = 'MiddleCenter'
    $main.Controls.Add($board,0,0)
    $script:presence = New-ChatLabel ''
    $presenceRow = New-ChatGrid
    [void]$presenceRow.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Percent',100))
    [void]$presenceRow.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Absolute',174))
    [void]$presenceRow.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new('Absolute',174))
    $presenceRow.Controls.Add($presence,0,0)
    $script:deliveryLink = New-Object Windows.Forms.LinkLabel
    $deliveryLink.Text = 'Результати надсилання'
    $deliveryLink.TextAlign = 'MiddleRight'
    $deliveryLink.Dock = 'Fill'
    $deliveryLink.Add_LinkClicked({ Show-DeliveryResults })
    $deliveryLink.Visible = $false
    $presenceRow.Controls.Add($deliveryLink,1,0)
    $verifyLink = New-Object Windows.Forms.LinkLabel
    $verifyLink.Text = "Перевірити комп’ютери"
    $verifyLink.Dock = 'Fill'
    $verifyLink.TextAlign = 'MiddleRight'
    $verifyLink.Add_LinkClicked({ Show-PeerTrust })
    $presenceRow.Controls.Add($verifyLink,2,0)
    $main.Controls.Add($presenceRow,0,1)
    $script:grid = New-ChatGrid
    $main.Controls.Add($grid,0,2)
    foreach ($width in 20,20,0,20,20,0,20) {
        if ($width) { [void]$grid.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Percent',$width))) }
        else { [void]$grid.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Absolute',20))) }
    }
    foreach ($row in 0,1) { [void]$grid.RowStyles.Add((New-Object Windows.Forms.RowStyle('Percent',50))) }
    foreach ($col in 0,1) {
        $empty = New-Object Windows.Forms.Panel
        $empty.BorderStyle = 'FixedSingle'
        $empty.Dock = 'Fill'
        $empty.AccessibleName = 'Вільне місце'
        $grid.Controls.Add($empty,$col,0)
    }
    $script:seatFingerprints = @{}
    $script:buttons = @{board=$board}
    foreach ($seat in $seats) {
        $button = New-Object Windows.Forms.CheckBox
        $button.Appearance = 'Button'
        $button.Dock = 'Fill'
        $button.TextAlign = 'MiddleCenter'
        $button.Add_CheckedChanged({ Update-Send })
        $buttons[$seat.Name] = $button
        $grid.Controls.Add($button,$seat.X,$seat.Y)
        if ($seat.Name -eq 'Олег') { $grid.SetRowSpan($button,2) }
    }
    $script:caption = New-ChatGrid
    [void]$caption.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Percent',100)))
    [void]$caption.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Absolute',112)))
    $caption.Controls.Add((New-ChatLabel 'Повідомлення'),0,0)
    $script:counter = New-ChatLabel ''
    $counter.TextAlign = 'MiddleRight'
    $caption.Controls.Add($counter,1,0)
    $main.Controls.Add($caption,0,3)
    $script:draft = New-Object Windows.Forms.TextBox
    $draft.Multiline = $true
    $draft.AcceptsReturn = $true
    $draft.ScrollBars = 'Vertical'
    $draft.Dock = 'Fill'
    $draft.AccessibleName = 'Повідомлення'
    $draft.Add_TextChanged({ Update-Send })
    $draft.Add_KeyDown({
        param($src,$e)
        if ($e.Control -and $e.KeyCode -eq 'Enter') {
            $e.SuppressKeyPress = $true
            if ($send.Enabled) { Submit-Draft }
        }
    })
    $main.Controls.Add($draft,0,4)
    $footer = New-ChatGrid
    [void]$footer.RowStyles.Add((New-Object Windows.Forms.RowStyle('Percent',100)))
    [void]$footer.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Percent',100)))
    [void]$footer.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Absolute',140)))
    $main.Controls.Add($footer,0,5)
    $script:status = New-ChatLabel ''
    $footer.Controls.Add($status,0,0)
    $script:send = New-Object Windows.Forms.Button
    $send.Text = 'Надіслати'
    $send.Dock = 'Fill'
    $send.Add_Click({ Submit-Draft })
    $actions = New-Object Windows.Forms.Panel
    $actions.Dock = 'Fill'
    $actions.Margin = 0
    $footer.Controls.Add($actions,1,0)
    $actions.Controls.Add($send)
    $script:write = New-Object Windows.Forms.Button
    $write.Text = 'Написати'
    $write.Dock = 'Fill'
    $write.Add_Click({ Set-ComposerMode $true })
    $actions.Controls.Add($write)
    $script:deliveryText = New-Object Windows.Forms.TextBox
    $deliveryText.AccessibleName = 'Результати за отримувачами'
    $deliveryText.Multiline = $true
    $deliveryText.ReadOnly = $true
    $deliveryText.ScrollBars = 'Vertical'
    $deliveryText.Dock = 'Fill'
    $main.Controls.Add($deliveryText,0,6)
    $form.Add_FormClosing({ param($src,$e)
        if ($e.CloseReason -eq 'UserClosing') { $e.Cancel = $true; $src.Hide() }
    })
    Set-ComposerMode $false
    Update-Seats
}

function Show-Incoming($message) {
    $toast = New-Object OfficeChatToast
    $toast.Font = $form.Font
    $toast.FormBorderStyle = 'FixedSingle'
    $toast.ControlBox = $false
    $toast.ShowInTaskbar = $false
    $toast.TopMost = $true
    $toast.StartPosition = 'Manual'
    $area = [Windows.Forms.Screen]::FromPoint([Windows.Forms.Cursor]::Position).WorkingArea
    $toast.Size = New-Object Drawing.Size(360,152)
    $toast.Location = New-Object Drawing.Point(($area.Right-$toast.Width-12),($area.Bottom-$toast.Height-12))
    $toast.Padding = 12
    $body = New-ChatLabel ([string]$message.text)
    $body.TextAlign = 'TopLeft'
    $body.AutoEllipsis = $true
    $heading = New-ChatLabel ([string]$message.sender_name)
    $heading.Dock = 'Top'
    $heading.Height = 28
    $toast.Controls.Add($body)
    $toast.Controls.Add($heading)
    $toast.Show()
    $toast
}

function Receive-Incoming {
    if ($script:popup) {
        if ([datetime]::UtcNow -lt $script:hideAt) { return }
        $script:popup.Dispose()
        $script:popup = $null
    }
    foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $MailboxRoot 'inbox') -Filter '*.json' -File | Sort-Object LastWriteTimeUtc,Name)) {
        try {
            $message = Read-ChatJson $file.FullName
            $uuid = [guid]::Empty
            if (-not [guid]::TryParseExact($file.BaseName,'D',[ref]$uuid) -or $file.BaseName -ne $message.id -or
                -not $message.sender_id -or -not $message.sender_name -or $message.text -isnot [string]) { continue }
            $seen = Join-Path $MailboxRoot "seen\$($file.Name)"
            if ([IO.File]::Exists($seen)) { continue }
            $script:popup = Show-Incoming $message
            $script:hideAt = [datetime]::UtcNow.AddSeconds(8)
            [IO.File]::Move($file.FullName,$seen)
            return
        } catch { if ($script:popup) { return } }
    }
}

function Show-History {
    $history = New-Object Windows.Forms.Form
    $history.Text = 'Отримані повідомлення'
    $history.Size = New-Object Drawing.Size(540,420)
    $view = New-Object Windows.Forms.TextBox
    $view.Font = $form.Font
    $view.Multiline = $true
    $view.ReadOnly = $true
    $view.Dock = 'Fill'
    $view.ScrollBars = 'Vertical'
    $items = foreach ($file in (Get-ChildItem -LiteralPath (Join-Path $MailboxRoot 'seen') -Filter '*.json' -File | Sort-Object LastWriteTimeUtc -Descending)) {
        try {
            $message = Read-ChatJson $file.FullName
            "$($message.sender_name)`r`n$($message.text)"
        } catch {}
    }
    $view.Text = if ($items) { $items -join "`r`n`r`n" } else { 'Отриманих повідомлень ще немає.' }
    $history.Controls.Add($view)
    $history.Show()
}

if ($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference = 'Stop'
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        $path = [IO.Path]::GetFullPath($MailboxRoot).TrimEnd('\').ToUpperInvariant()
        $scope = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($path)),0,8)
    } finally { $hash.Dispose() }
    $mutex = New-Object Threading.Mutex($false,"Local\CodexOfficeChat-$sid-$scope")
    try {
        try { $owner = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owner = $true }
        if (-not $owner) { return }
        Initialize-Mailbox
        [Windows.Forms.Application]::EnableVisualStyles()
        New-ChatWindow
        $menu = New-Object Windows.Forms.ContextMenuStrip
        $open = { Open-ChatWindow }
        [void]$menu.Items.Add('Написати повідомлення',$null,$open)
        [void]$menu.Items.Add('Отримані повідомлення',$null,{ Show-History })
        $recover = $menu.Items.Add('Відновити непідтверджене повідомлення',$null,{ Restore-ChatDraft })
        $menu.Add_Opening({
            $failed = Join-Path $MailboxRoot 'failed'
            $recover.Enabled = [IO.Directory]::Exists($failed) -and [bool]([IO.Directory]::EnumerateFiles($failed,'*.json') | Select-Object -First 1)
        })
        if ($env:LED_FORGE_TRAY_RESTART) {
            [void]$menu.Items.Add('Перезапустити агента',$null,{ param($item,$eventArgs)
                if (Request-LocalAgentRestart) { $item.Enabled = $false }
            })
        }
        [void]$menu.Items.Add('Вийти',$null,{ [Windows.Forms.Application]::ExitThread() })
        [void]$menu.Items.Add("Перевірити комп’ютери",$null,{ Show-PeerTrust })
        [void]$menu.Items.Add('Підключити табло',$null,{
            try {
                . (Join-Path $PSScriptRoot 'agent_update.ps1')
                . (Join-Path $PSScriptRoot 'agent_chat_ipc.ps1')
                Show-AgentChatPairing $MailboxRoot (Join-Path $PSScriptRoot 'controller-endpoint.json')
            } catch { [void][Windows.Forms.MessageBox]::Show('Не вдалося відкрити форму підключення табло.') }
        })
        $tray = New-Object Windows.Forms.NotifyIcon
        $tray.Text = 'Офісний чат'
        $tray.Icon = [Drawing.SystemIcons]::Information
        $tray.ContextMenuStrip = $menu
        $tray.Add_MouseClick({ param($src,$e)
            if ($e.Button -eq 'Left') { & $open }
        })
        $tray.Visible = $true
        $timer = New-Object Windows.Forms.Timer
        $timer.Interval = 1500
        $timer.Add_Tick({ if ($ChatStopEvent -and $ChatStopEvent.WaitOne(0)) { [Windows.Forms.Application]::ExitThread(); return }; if ($form.Visible) { Update-Seats; Update-DeliveryView }; Receive-Incoming })
        $timer.Start()
        [Windows.Forms.Application]::Run()
    } finally {
        foreach ($item in $timer,$tray,$menu,$popup,$trustWindow,$form) { if ($item) { $item.Dispose() } }
        if ($owner) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}
