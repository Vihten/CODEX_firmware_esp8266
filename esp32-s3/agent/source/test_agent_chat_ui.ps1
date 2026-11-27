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
function Save-ChatPreview($path,$window=$form) {
    $bitmap = New-Object Drawing.Bitmap($window.Width,$window.Height)
    try { $window.DrawToBitmap($bitmap,[Drawing.Rectangle]::new(0,0,$bitmap.Width,$bitmap.Height)); $bitmap.Save($path) } finally { $bitmap.Dispose() }
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
    $pin = 'A1' * 32
    function Copy-ChatIdentity([string]$value) { $script:copiedIdentity = $value }
    $ownPin = 'C3' * 32
    $peer = @{id='00000002';name='Dimasik';fingerprint=$pin}
    $snapshot = @{self_id='00000001';self_fingerprint=$ownPin;agents=$agents;candidates=@($peer)}
    Write-Fixture 'roster.json' $snapshot
    Update-Seats
    Check (-not $buttons['Дімасік'].Enabled -and $board.Enabled) 'Legacy unpinned desktop became selectable'
    Show-PeerTrust
    [Windows.Forms.Application]::DoEvents()
    Assert-VisibleLayout $trustWindow
    if ($PreviewPath) {
        $trustPath = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($PreviewPath))) ([IO.Path]::GetFileNameWithoutExtension($PreviewPath) + '-trust.png')
        Save-ChatPreview $trustPath $trustWindow
    }
    Check ($ownFingerprint.Text -ceq (Format-PeerFingerprint $ownPin) -and $ownFingerprint.ReadOnly) 'Own fingerprint unavailable'
    $expectedCode = 'AF98 F090 F8D2'
    Check ($peerList.SelectedItem.Identity.id -ceq $peer.id -and $pairCode.Text -ceq $expectedCode) 'Pair comparison vector'
    Check (-not (Get-PeerComparisonCode $pin $pin) -and -not (Get-PeerComparisonCode 'bad' $pin)) 'Invalid pins produced a code'
    Check ((Get-PeerComparisonCode $ownPin ('A1' * 31 + 'A0')) -cne $expectedCode) 'Last byte of pin omitted'
    Check ($codesMatch.Enabled -and -not $codesMatch.Checked -and -not $approvePeer.Enabled) 'Approval requires explicit comparison'
    $copyIdentity.PerformClick()
    Check ($copiedIdentity -ceq ('ID 00000001  ' + (Format-PeerFingerprint $ownPin))) 'Clipboard identity missing ID or grouped fingerprint'
    $codesMatch.Checked = $true
    Refresh-PeerTrust
    Check (-not $codesMatch.Checked -and -not $approvePeer.Enabled) 'Refresh retained confirmation'
    $codesMatch.Checked = $true
    Check ($approvePeer.Enabled) 'Compared pair code did not enable approval'
    $other = @{id='00000006';name='Nestor';fingerprint='E5' * 32}
    $snapshot.candidates = @($other,$peer)
    Write-Fixture 'roster.json' $snapshot
    # Pump the real dialog timer. The map is still hidden.
    $deadline = [datetime]::UtcNow.AddSeconds(5)
    while ($peerList.Items.Count -ne 2 -and [datetime]::UtcNow -lt $deadline) {
        [Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 25
    }
    Check ($peerList.Items.Count -eq 2 -and $peerList.SelectedIndex -eq 1 -and $codesMatch.Checked -and $approvePeer.Enabled) 'Live discovery lost the selected peer or its comparison'
    Check (-not [IO.File]::Exists((Get-TrustBindingPath $peer.id)) -and -not [IO.File]::Exists((Get-TrustBindingPath $other.id))) 'Discovery saved trust'
    foreach ($change in 'localPin','localId','name','pin','id','conflict','removed','unreadable') {
        $savedName = $peer.name
        $savedId = $peer.id
        switch ($change) {
            'localPin' { $snapshot.self_fingerprint = '99' * 32 }
            'localId' { $snapshot.self_id = '12345678' }
            'name' { $peer.name = 'Another name' }
            'pin' { $peer.fingerprint = '88' * 32 }
            'id' { $peer.id = '12345678' }
            'conflict' { $snapshot.candidates += @{id=$peer.id;name=$peer.name;fingerprint='77' * 32} }
            'removed' { $snapshot.candidates = @($other) }
        }
        Write-Fixture 'roster.json' $snapshot
        if ($change -eq 'unreadable') { [IO.File]::WriteAllText((Join-Path $testRoot 'roster.json'),'{') }
        Refresh-PeerTrust -Automatic
        Check (-not $codesMatch.Checked -and -not $approvePeer.Enabled -and $peerList.SelectedIndex -eq -1) "Stale selection after $change"
        $snapshot.self_fingerprint = $ownPin
        $snapshot.self_id = '00000001'
        $peer.name = $savedName
        $peer.id = $savedId
        $peer.fingerprint = $pin
        $snapshot.candidates = @($other,$peer)
        Write-Fixture 'roster.json' $snapshot
        Refresh-PeerTrust -Automatic
        Check ($peerList.SelectedIndex -eq -1 -and -not $approvePeer.Enabled) "Reappearance restored approval after $change"
        $peerList.SelectedIndex = 1
        $codesMatch.Checked = $true
    }
    $snapshot.candidates = @($peer)
    Write-Fixture 'roster.json' $snapshot
    Refresh-PeerTrust -Automatic
    $bindingPath = Get-TrustBindingPath $peer.id
    $peer.fingerprint = 'B2' * 32
    Write-Fixture 'roster.json' $snapshot
    $approvePeer.PerformClick()
    Check (-not [IO.File]::Exists($bindingPath) -and -not $approvePeer.Enabled) 'Stale verification pinned a changed key'
    $peer.fingerprint = $pin
    Write-Fixture 'roster.json' $snapshot
    Refresh-PeerTrust
    $codesMatch.Checked = $true
    $approvePeer.PerformClick()
    Check ((Test-BindingMatches $peer) -and -not $approvePeer.Enabled) 'First approval did not persist once'
    Check (-not $buttons['Дімасік'].Enabled) 'Approval bypassed runtime verification'
    $agents[1].fingerprint = $pin
    $agents[1].trusted = $true
    $agents[2].fingerprint = 'D4' * 32
    $agents[2].trusted = $true
    Write-Fixture 'trusted\00000003.json' $agents[2]
    Write-Fixture 'roster.json' $snapshot
    Update-Seats
    Check ($buttons['Дімасік'].Enabled) 'Verified online peer not selectable'
    $buttons['Дімасік'].Checked = $true
    $peer.fingerprint = 'B2' * 32
    Write-Fixture 'roster.json' $snapshot
    Refresh-PeerTrust
    Update-Seats
    Check (-not $pairCode.Text -and -not $codesMatch.Enabled) 'Changed binding displayed a usable pairing code'
    Check (-not $approvePeer.Enabled -and -not $buttons['Дімасік'].Checked -and -not $buttons['Дімасік'].Enabled) 'Changed identity accepted'
    Check ((Read-ChatJson $bindingPath).fingerprint -ceq $pin) 'Pin overwritten'
    $peer.fingerprint = $pin
    Write-Fixture 'roster.json' $snapshot
    $snapshot.candidates = @()
    Write-Fixture 'roster.json' $snapshot
    $closedTimer = $trustWindow.Tag
    $trustWindow.Close()
    Check (-not $closedTimer.Enabled) 'Closed dialog kept polling'
    Show-PeerTrust
    Refresh-PeerTrust -Automatic
    Check ($peerList.SelectedItem.Identity.id -ceq $peer.id -and $pairCode.Text -ceq $expectedCode -and -not $codesMatch.Enabled -and -not $approvePeer.Enabled) 'Previously approved peer must still display the comparison code'
    $reverse = @{self_id=$peer.id;self_fingerprint=$pin;candidates=@(@{id=$snapshot.self_id;name='Oleksii';fingerprint=$ownPin});agents=@()}
    Write-Fixture 'roster.json' $reverse
    Refresh-PeerTrust
    Check ($codesMatch.Enabled -and -not $approvePeer.Enabled -and $pairCode.Text -ceq $expectedCode) 'Other PC did not independently display the same code'
    $codesMatch.Checked = $true
    $approvePeer.PerformClick()
    Check ((Test-BindingMatches $reverse.candidates[0]) -and $pairCode.Text -ceq $expectedCode) 'Second PC did not save full pin'
    $snapshot.candidates = @($peer)
    Write-Fixture 'roster.json' $snapshot
    $trustWindow.Close()
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
    Check ($deliveryText.Visible -and $deliveryText.Text.Contains('Дімасік — очікує') -and $deliveryText.Text.Contains('Табло — очікує')) 'Missing queued results'
    [void][IO.Directory]::CreateDirectory((Join-Path $testRoot 'deliveries'))
    $targets = @(@{id='00000002';state='delivered'},@{id='board';state='pending'})
    Write-Fixture "deliveries/$($envelope.id).json" @{id=$envelope.id;targets=$targets}
    Update-DeliveryView
    Check ($deliveryText.Text.Contains('Дімасік — доставлено') -and $deliveryText.Text.Contains('Табло — очікує')) 'Per-recipient status mismatch'
    if ($PreviewPath) {
        $resultPath = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($PreviewPath))) ([IO.Path]::GetFileNameWithoutExtension($PreviewPath) + '-delivery.png')
        Save-ChatPreview $resultPath
    }
    $targets[1].state = 'failed'
    Write-Fixture "deliveries/$($envelope.id).json" @{id=$envelope.id;targets=$targets}
    Update-DeliveryView
    Check ($deliveryText.Text.Contains('Дімасік — доставлено') -and $deliveryText.Text.Contains('Табло — не доставлено')) 'Mixed terminal receipt states'
    Set-ComposerMode $false
    Check ($form.Height -eq $compactHeight -and $deliveryLink.Visible -and -not $deliveryText.Visible) 'Results enlarged compact map'
    Assert-VisibleLayout $form
    Show-DeliveryResults
    Assert-VisibleLayout $form
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
    Write-Fixture "failed/$failedId.json" @{id=$failedId;recipient_ids=@('00000005','board');text='recover me'}
    Write-Fixture "deliveries/$failedId.json" @{id=$failedId;targets=@(@{id='00000005';state='uncertain'},@{id='board';state='delivered'})}
    $draft.Clear()
    Restore-ChatDraft
    Check ($deliveryText.Text.Contains('Табло — доставлено') -and $deliveryText.Text.Contains('доставку не підтверджено') -and -not $board.Enabled) 'Partial recovery hid or allowed resending delivered recipient'
    Check ($draft.Text -ceq 'recover me' -and -not $board.Checked -and -not $send.Enabled) 'Failed send recovery'
    $stop = New-Object Threading.ManualResetEvent($true)
    try { & (Join-Path $PSScriptRoot 'agent_chat_ui.ps1') -MailboxRoot $testRoot -ChatStopEvent $stop }
    finally { $stop.Dispose() }
    'Windows tray chat UI tests passed'
} finally {
    if ($trustWindow) { $trustWindow.Dispose() }
    if ($popup) { $popup.Dispose() }
    if ($form) { $form.Dispose() }
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ([IO.Path]::GetDirectoryName($testRoot).TrimEnd('\') -eq $temp.TrimEnd('\') -and
        [IO.Path]::GetFileName($testRoot) -match '^office-chat-ui-[0-9a-f-]{36}$' -and [IO.Directory]::Exists($testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
