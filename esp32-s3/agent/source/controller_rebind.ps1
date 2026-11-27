function Get-ControllerRebindHealth {
    param([string]$DirectHost, [int]$DirectHttpPort)

    $request = [Net.HttpWebRequest]::Create("http://${DirectHost}:$DirectHttpPort/health")
    $request.Method = 'GET'
    $request.Proxy = $null
    $request.AllowAutoRedirect = $false
    $request.KeepAlive = $false
    $request.Timeout = 3000
    $request.ReadWriteTimeout = 1000
    try {
        $response = $request.GetResponse()
        try {
            if ([int]$response.StatusCode -ne 200) { throw 'Controller health must return HTTP 200.' }
            $reader = [IO.StreamReader]::new($response.GetResponseStream())
            try {
                $buffer = [char[]]::new(4096)
                $count = $reader.ReadBlock($buffer, 0, $buffer.Length)
                if ($count -eq $buffer.Length) { throw 'Controller health response is too large.' }
                [string]::new($buffer, 0, $count)
            }
            finally { $reader.Dispose() }
        }
        finally { $response.Dispose() }
    }
    finally { $request.Abort() }
}

function New-ControllerRebindState {
    param(
        [Parameter(Mandatory = $true)] [string]$DirectHost,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('\A[0-9A-Fa-f]{6}\z')] [string]$DeviceId,
        [ValidateRange(1, 65535)] [int]$DirectPort = 4210,
        [ValidateRange(1, 65535)] [int]$DirectHttpPort = 80,
        [ValidatePattern('\A[0-9A-Fa-f]{8}\z')] [string]$AccountFingerprint = '53197668'
    )

    $ip = $null
    if (-not [Net.IPAddress]::TryParse($DirectHost, [ref]$ip) -or
        $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
        $ip.ToString() -cne $DirectHost) {
        throw 'Explicit controller selection requires a canonical private IPv4 address.'
    }
    $bytes = $ip.GetAddressBytes()
    $private = $bytes[0] -eq 10 -or
        ($bytes[0] -eq 172 -and $bytes[1] -ge 16 -and $bytes[1] -le 31) -or
        ($bytes[0] -eq 192 -and $bytes[1] -eq 168)
    if (-not $private) { throw 'Explicit controller selection requires a private IPv4 address.' }

    # Verify only the explicitly supplied endpoint; never discover or send telemetry here.
    $body = Get-ControllerRebindHealth -DirectHost $DirectHost -DirectHttpPort $DirectHttpPort
    $identities = [regex]::Matches($body, '(?:\A|\s)controllerId=([0-9A-Fa-f]{6})(?=\s|\z)')
    if ($identities.Count -ne 1 -or $identities[0].Groups[1].Value -ine $DeviceId) {
        throw "Controller identity mismatch at ${DirectHost}:$DirectHttpPort; expected $DeviceId. Installation unchanged."
    }
    [pscustomobject]@{
        Seed = $DirectHost
        Account = $AccountFingerprint.ToUpperInvariant()
        Port = $DirectPort
        HttpPort = $DirectHttpPort
        Host = $DirectHost
        Device = $DeviceId.ToUpperInvariant()
    }
}
