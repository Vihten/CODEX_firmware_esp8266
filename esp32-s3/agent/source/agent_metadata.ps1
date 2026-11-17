function Get-AgentMetadata {
    param(
        [string]$Name,
        [string]$ColorHex6,
        [string]$ClientId = "$env:COMPUTERNAME-$env:USERNAME"
    )

    foreach ($character in $Name.ToCharArray()) {
        if ([char]::IsControl($character)) { throw 'Agent name must not contain control characters.' }
    }
    if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $env:COMPUTERNAME }
    if ([string]::IsNullOrWhiteSpace($Name)) { $Name = $ClientId }
    foreach ($character in $Name.ToCharArray()) {
        if ([char]::IsControl($character)) { throw 'Agent name must not contain control characters.' }
    }
    $Name = $Name.Trim()
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    # Validate UTF-16 first, then shorten without splitting a surrogate pair.
    [void]$utf8.GetByteCount($Name)
    while ($utf8.GetByteCount($Name) -gt 48) {
        $length = $Name.Length - 1
        if ([char]::IsLowSurrogate($Name[$length])) { $length-- }
        $Name = $Name.Substring(0, $length)
    }
    if ([string]::IsNullOrWhiteSpace($ColorHex6)) {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $hash = $sha.ComputeHash($utf8.GetBytes($ClientId))
            $clientHash = -join ($hash[0..3] | ForEach-Object { $_.ToString('X2') })
            $palette = @('FF4545', 'FFE04A', '39D965', '35DDE0', '4A78FF', 'DF55E8')
            $ColorHex6 = $palette[([Convert]::ToUInt32($clientHash, 16) % 6)]
        }
        finally { $sha.Dispose() }
    }
    if ($ColorHex6 -cnotmatch '\A[0-9A-Fa-f]{6}\z') {
        throw 'Color must contain exactly six hexadecimal digits (without #).'
    }
    [pscustomobject]@{
        Name = $Name
        ColorHex6 = $ColorHex6.ToUpperInvariant()
        NameUtf8Hex = -join ($utf8.GetBytes($Name) | ForEach-Object { $_.ToString('X2') })
    }
}
