#requires -Version 5.1
param([string]$PreviewPath)
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("office-chat-ui-" + [guid]::NewGuid())
. (Join-Path $PSScriptRoot 'agent_chat_ui.ps1') -MailboxRoot $testRoot
function Check($condition,[string]$description) { if (-not $condition) { throw $description } }
function Assert-VisibleLayout($parent) {
    foreach ($child in $parent.Controls) {
        if (-not $child.Visible) { continue }
        Check ($child.Width -gt 0 -and $child.Height -gt 0 -and $parent.ClientRectangle.Contains($child.Bounds)) "Clipped $($child.GetType().Name) '$($child.Text)' $($child.Bounds) inside $($parent.ClientRectangle)"
        if ($child -is [Windows.Forms.Button]) {
            $label = [Windows.Forms.TextRenderer]::MeasureText($child.Text,$child.Font)
            Check ($child.ClientSize.Width -ge $label.Width+8 -and $child.ClientSize.Height -ge $label.Height+4) "Button caption clipped: $($child.Text)"
        }
        Assert-VisibleLayout $child
    }
}
function Save-ChatPreview($path) {
    $bitmap = New-Object Drawing.Bitmap($form.Width,$form.Height)
    try { $form.DrawToBitmap($bitmap,[Drawing.Rectangle]::new(0,0,$bitmap.Width,$bitmap.Height)); $bitmap.Save($path) } finally { $bitmap.Dispose() }
}
function Write-Fixture($name,$object) {
    [IO.File]::WriteAllText((Join-Path $testRoot $name),($object | ConvertTo-Json -Depth 5))
}
try {
    Initialize-Mailbox
    [Windows.Forms.Application]::EnableVisualStyles()
    New-ChatWindow
    Check (-not $board.Enabled -and -not $send.Enabled) 'Missing snapshot enabled sending'
    $agents = @(
        @{id='00000001';name='Oleksii';online=$true}
        @{id='00000002';name='Dimasik';online=$true}
        @{id='00000003';name='Sasha';online=$false}
        @{id='00000004';name='Vlad';online=$true}
        @{id='board';name='Board';online=$true}
    )
    $snapshot = @{self_id='00000001'; agents=$agents}
    Write-Fixture 'roster.json' $snapshot
    Update-Seats
    Check (-not $buttons['Олексій'].Enabled -and $buttons['Олексій'].Text.EndsWith('Ви')) 'Self was selectable'
    Check (-not $buttons['Саша'].Enabled -and $buttons['Саша'].Text.Contains('Поза мережею')) 'Offline seat'
    Check ($board.Enabled -and -not $board.Checked) 'Board must require explicit selection'
    $expected = @('0,1:Влад','1,1:Олексій','3,0:Нестор','4,0:Дімасік','3,1:Саша','4,1:Микола','6,0:Олег')
    foreach ($entry in $expected) {
        $position,$name = $entry -split ':'
        $x,$y = $position -split ','
        Check ($grid.GetControlFromPosition($x,$y) -eq $buttons[$name]) "Seat position $name"
    }
    Check ($grid.GetRowSpan($buttons['Олег']) -eq 2) 'Separate desk span'
    foreach ($x in 0,1) { Check ($grid.GetControlFromPosition($x,0) -is [Windows.Forms.Panel]) 'Upper empty desk' }
    $buttons['Дімасік'].Checked = $true
    $board.Checked = $true
    Open-ChatWindow
    [Windows.Forms.Application]::DoEvents()
    Check ($board.Text -ceq 'Табло' -and $write.Text -ceq 'Написати' -and $write.Visible -and $write.Enabled) 'Compact map action'
    Check (-not $draft.Visible -and -not $send.Visible) 'Composer visible on initial map'
    $area = [Windows.Forms.Screen]::FromControl($form).WorkingArea
    Check ($form.Right -eq $area.Right-12 -and $form.Bottom -eq $area.Bottom-12) 'Compact map corner placement'
    Assert-VisibleLayout $form
    if ($PreviewPath) {
        $compactPath = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($PreviewPath))) ([IO.Path]::GetFileNameWithoutExtension($PreviewPath) + '-compact.png')
        Save-ChatPreview $compactPath
    }
    $compactHeight = $form.Height
    $write.PerformClick()
    Check ($draft.Visible -and $form.Height -gt $compactHeight -and $form.Bottom -eq $area.Bottom-12) 'Write action did not open anchored editor'
    $draft.Text = "A`tB"
    Check (-not $send.Enabled) 'Tab must not enter the transport queue'
    $draft.Text = 'ї' * 257
    Check (-not $send.Enabled) '513+ UTF-8 bytes accepted'
    $draft.Text = 'ї' * 256
    Check ($send.Enabled) '512 UTF-8 bytes rejected'
    $draft.Text = "Привіт, командо! 👋`r`nДругий рядок."
    $form.Show()
    [Windows.Forms.Application]::DoEvents()
    Check ([Math]::Abs($board.Left + $board.Width/2 - $board.Parent.ClientSize.Width/2) -le 2) 'Board off center'
    Assert-VisibleLayout $form
    Check ($send.Visible -and $send.Text -ceq 'Надіслати' -and $form.Right -eq $area.Right-12) 'Composer action or anchor'
    if ($PreviewPath) { Save-ChatPreview $PreviewPath }
    $send.PerformClick()
    $queued = @(Get-ChildItem -LiteralPath (Join-Path $testRoot 'outbox') -File)
    Check ($queued.Count -eq 1) 'Expected exactly one committed envelope'
    $envelope = Read-ChatJson $queued[0].FullName
    Check ($envelope.id -eq $queued[0].BaseName -and $envelope.text -ceq "Привіт, командо! 👋`nДругий рядок.") 'Unicode envelope'
    Check (($envelope.recipient_ids -join ',') -ceq '00000002,board') 'Recipient IDs'
    $draft.Text = 'Чернетка'
    $agents[1].id = '00000005'
    Write-Fixture 'roster.json' $snapshot
    Submit-Draft
    Check (-not $buttons['Дімасік'].Checked -and $draft.Text -ceq 'Чернетка') 'Roster change redirected/cleared draft'
    $form.Close()
    Check (-not $form.Visible -and -not $form.IsDisposed) 'Close must hide composer'
    $snapshot.self_name = 'Vlad'
    $snapshot.agents = @($agents | Where-Object { $_.id -notin '00000001','00000004' })
    Write-Fixture 'roster.json' $snapshot
    Open-ChatWindow
    Check (-not $draft.Visible -and $draft.Text -ceq 'Чернетка') 'Reopen compact with draft retained'
    Check (-not $buttons['Влад'].Enabled -and $buttons['Влад'].Text.EndsWith('Ви')) 'Self name fallback'
    $form.Hide()
    $id = [guid]::NewGuid().ToString()
    Write-Fixture "inbox\$id.json" @{id=$id;sender_id='00000005';sender_name='Дімасік';text='Вхідне 👋'}
    Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class ChatUIFocus { [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow(); }'
    $focus = [ChatUIFocus]::GetForegroundWindow()
    Receive-Incoming
    [Windows.Forms.Application]::DoEvents()
    Check ($popup.Visible -and -not $form.Visible) 'Popup opened composer'
    Check ($focus -eq [ChatUIFocus]::GetForegroundWindow()) 'Popup stole foreground'
    $screen = [Windows.Forms.Screen]::FromControl($popup).WorkingArea
    Check ($popup.Right -eq $screen.Right-12 -and $popup.Bottom -eq $screen.Bottom-12) 'Popup corner placement'
    Check ([IO.File]::Exists((Join-Path $testRoot "seen\$id.json"))) 'Inbox consumption'
    $script:hideAt = [datetime]::MinValue
    Receive-Incoming
    Check (-not $popup) 'Popup did not expire'
    [void][IO.Directory]::CreateDirectory((Join-Path $testRoot 'failed'))
    $failedId = [guid]::NewGuid().ToString()
    Write-Fixture "failed/$failedId.json" @{id=$failedId;recipient_ids=@('board');text='recover me'}
    $draft.Clear()
    Restore-ChatDraft
    Check ($draft.Text -ceq 'recover me' -and -not $board.Checked -and -not $send.Enabled) 'Failed send recovery'
    $stop = New-Object Threading.ManualResetEvent($true)
    try { & (Join-Path $PSScriptRoot 'agent_chat_ui.ps1') -MailboxRoot $testRoot -ChatStopEvent $stop }
    finally { $stop.Dispose() }
    'Windows tray chat UI tests passed'
} finally {
    if ($popup) { $popup.Dispose() }
    if ($form) { $form.Dispose() }
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ([IO.Path]::GetDirectoryName($testRoot).TrimEnd('\') -eq $temp.TrimEnd('\') -and
        [IO.Path]::GetFileName($testRoot) -match '^office-chat-ui-[0-9a-f-]{36}$' -and [IO.Directory]::Exists($testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
