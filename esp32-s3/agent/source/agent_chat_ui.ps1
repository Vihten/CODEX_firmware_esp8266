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

function Update-Seats {
    $snapshot = $null
    try { $snapshot = Read-ChatJson (Join-Path $MailboxRoot 'roster.json') } catch {}
    $rows = @($snapshot.agents)
    if ($snapshot.self_id -and $snapshot.self_name -and -not ($rows | Where-Object { $_.id -eq $snapshot.self_id })) {
        $rows += [pscustomobject]@{id=$snapshot.self_id;name=$snapshot.self_name;online=$true}
    }
    foreach ($seat in $seats) {
        $button = $buttons[$seat.Name]
        $names = @($seat.Name) + $seat.Aliases
        $peers = @($rows | Where-Object { $_.id -and $_.id -ne 'board' -and $names -contains ([string]$_.name).Trim() })
        $id = $null
        $state = 'Поза мережею'
        $enabled = $false
        if ($peers.Count -gt 1) { $state = "Неоднозначне ім’я" }
        elseif ($peers.Count -eq 1) {
            $id = [string]$peers[0].id
            if ($id -eq $snapshot.self_id) { $state = 'Ви' }
            elseif ($peers[0].online -is [bool] -and $peers[0].online) {
                $state = 'У мережі'
                $enabled = -not [string]::IsNullOrWhiteSpace($snapshot.self_id)
            }
        }
        if (-not $enabled -or $id -ne $button.Tag) { $button.Checked = $false }
        $button.Tag = $id
        $button.Enabled = $enabled
        $button.Text = $seat.Name + [Environment]::NewLine + $state
    }
    $boardRows = @($snapshot.agents | Where-Object { $_.id -ceq 'board' })
    $board.Enabled = $snapshot.self_id -and $boardRows.Count -eq 1 -and $boardRows[0].online -is [bool] -and $boardRows[0].online
    if (-not $board.Enabled) { $board.Checked = $false }
    if (-not $snapshot.self_id) { $presence.Text = 'Очікуємо список учасників' }
    else { $presence.Text = 'Оберіть отримувачів' }
    Update-Send
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
    if ($msg.text -isnot [string]) { return }
    Open-ChatWindow
    foreach ($button in $buttons.Values) { $button.Checked = $false }
    $draft.Text = $msg.text.Replace([string][char]10,[Environment]::NewLine)
    $script:recoveredPath = $entry.FullName
    Set-ComposerMode $true
    $status.Text = 'Доставку не підтверджено. Перевірте отримувачів перед повтором.'
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
    foreach ($height in 32,28,132,36,-1,42) {
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
    $main.Controls.Add($presence,0,1)
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
        [void]$menu.Items.Add('Вийти',$null,{ [Windows.Forms.Application]::ExitThread() })
        [void]$menu.Items.Add('Підключити чат',$null,{
            try {
                . (Join-Path $PSScriptRoot 'agent_update.ps1')
                . (Join-Path $PSScriptRoot 'agent_chat_ipc.ps1')
                Show-AgentChatPairing $MailboxRoot (Join-Path $PSScriptRoot 'controller-endpoint.json')
            } catch { [void][Windows.Forms.MessageBox]::Show('Не вдалося відкрити форму підключення.') }
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
        $timer.Add_Tick({ if ($ChatStopEvent -and $ChatStopEvent.WaitOne(0)) { [Windows.Forms.Application]::ExitThread(); return }; if ($form.Visible) { Update-Seats }; Receive-Incoming })
        $timer.Start()
        [Windows.Forms.Application]::Run()
    } finally {
        foreach ($item in $timer,$tray,$menu,$popup,$form) { if ($item) { $item.Dispose() } }
        if ($owner) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}
