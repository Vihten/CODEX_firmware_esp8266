[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'codex_lan_agent.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
foreach ($name in @('Get-AccountFingerprint', 'Update-AccountRemaining', 'Send-Heartbeat')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    . ([scriptblock]::Create($definition.Extent.Text))
}
function Assert($Value, [string]$Message) { if (-not $Value) { throw $Message } }
Assert ((Get-AccountFingerprint 'hello') -ceq '4F9F2CAB') 'FNV reference vector'
Assert ((Get-AccountFingerprint '  PAIRED@EXAMPLE.INVALID  ') -ceq 'B6C55623') 'Trim/lowercase reference vector'
$nonBmp = [char]0xD83D + [string][char]0xDE80 + '@example.invalid'
Assert ((Get-AccountFingerprint $nonBmp) -ceq '74B1E573') 'UTF-16 surrogate pair reference vector'
Assert ((Get-AccountFingerprint $null) -ceq '00000000') 'Null identity'
Assert ((Get-AccountFingerprint '   ') -ceq '00000000') 'Blank identity'
$AccountFingerprint = '53197668'; $clientHash = '12345678'; $clientId = 'test'
$CollectorHost = '127.0.0.1'; $Port = 4212; $DirectHost = '127.0.0.1'; $DirectPort = 4210
$metadata = @{ Name = 'test'; ColorHex6 = 'ABCDEF'; NameUtf8Hex = '74657374' }
$effectiveColorHex6 = 'ABCDEF'; $activity = 0.7
$weeklyTokens = $null; $weeklyTokensReady = $false; $dailyTokens = @()
$nextOriginHttpAt = [DateTime]::MaxValue
$codexActivity = 0.7; $claudeActivity = 0.1
$nextDirectHttpAt = [DateTime]::MaxValue
$nextWeeklyReportAt = [DateTime]::MaxValue
$nextDailyReportAt = [DateTime]::MaxValue
$packets = [Collections.Generic.List[string]]::new()
$udp = New-Object PSObject
$udp | Add-Member ScriptMethod Send {
    param($Bytes, $Length, $Address, $DestinationPort)
    $script:packets.Add([Text.Encoding]::UTF8.GetString($Bytes)); return $Length
}
$quotaFingerprint = '53197668'
Update-AccountRemaining 39.6
Send-Heartbeat
Assert ($packets[1] -ceq 'LFA3,53197668,12345678,70,40,53197668,ABCDEF,74657374') 'Verified wire schema'
Assert (($packets[0] | ConvertFrom-Json).remaining -eq -1) 'Legacy bridge must never carry quota'
Assert ($packets[2] -ceq 'LFX1,53197668,12345678,70,10') 'Origin supplement follows unchanged LFA3'
$quotaFingerprint = '00000000'
Update-AccountRemaining -1
Send-Heartbeat
Assert ($packets[4] -ceq 'LFA3,53197668,12345678,70,-1,00000000,ABCDEF,74657374') 'Unknown quota wire schema/activity preserved'
Write-Host 'PASS: FNV-1a UTF-16 vectors, normalization, verified/unknown LFA3, legacy quota suppression, activity preserved.'

# Exercise supplement scheduling through the normal heartbeat with isolated HTTP.
$originHttpSender = @{ Worker=$null; Pending=$null; Stopping=$null; Started=[DateTimeOffset]::MinValue }
$claudeHttpSender = @{ Worker=$null; Pending=$null; Stopping=$null; Started=[DateTimeOffset]::MinValue }
$originHttpCalls = [Collections.Generic.List[object]]::new()
function Start-ClaudeHttpSend($State, $Bytes, $Address, $HttpPort) {
    $script:originHttpCalls.Add(@{ State=$State; Packet=[Text.Encoding]::ASCII.GetString($Bytes) })
}
$nextOriginHttpAt = [DateTime]::MinValue
Send-Heartbeat
Send-Heartbeat
Assert ($originHttpCalls.Count -eq 1) 'Origin HTTP throttled across heartbeats'
Assert ($originHttpCalls[0].Packet -ceq 'LFX1,53197668,12345678,70,10') 'Origin HTTP matches UDP'
Assert ([object]::ReferenceEquals($originHttpCalls[0].State, $originHttpSender)) 'Origin uses dedicated sender state'
Assert (-not [object]::ReferenceEquals($originHttpSender, $claudeHttpSender)) 'Quota sender remains independent'
Assert ($nextOriginHttpAt -gt (Get-Date).AddSeconds(4)) 'Origin fallback scheduled five seconds ahead'
Remove-Item function:Start-ClaudeHttpSend
Write-Host 'PASS origin supplement schema, HTTP routing, independent sender state and five-second throttle.'
