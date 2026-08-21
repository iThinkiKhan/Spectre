param(
    [string]$SecretsPath = (Join-Path $PSScriptRoot '..\src\secrets.h'),
    [int[]]$PayloadSizes = @(300, 450, 480, 500, 520, 533, 600),
    [int]$TimeoutMs = 5000
)

$ErrorActionPreference = 'Stop'

function Get-SecretDefine([string]$Name) {
    $line = Select-String -LiteralPath $SecretsPath -Pattern "^\s*#define\s+$Name\s+" |
        Select-Object -First 1
    if (-not $line) { throw "Missing $Name in secrets header" }
    $value = $line.Line -replace "^\s*#define\s+$Name\s+", ''
    return $value.Trim().Trim('"')
}

function ConvertTo-MqttRemainingLength([int]$Length) {
    $bytes = [System.Collections.Generic.List[byte]]::new()
    do {
        $digit = $Length % 128
        $Length = [math]::Floor($Length / 128)
        if ($Length -gt 0) { $digit = $digit -bor 0x80 }
        $bytes.Add([byte]$digit)
    } while ($Length -gt 0)
    return $bytes.ToArray()
}

function Add-MqttString($Buffer, [string]$Value) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
    $Buffer.Add([byte](($bytes.Length -shr 8) -band 0xff))
    $Buffer.Add([byte]($bytes.Length -band 0xff))
    $Buffer.AddRange([byte[]]$bytes)
}

function Read-Exact($Stream, [int]$Length) {
    $bytes = [byte[]]::new($Length)
    $offset = 0
    while ($offset -lt $Length) {
        $read = $Stream.Read($bytes, $offset, $Length - $offset)
        if ($read -le 0) { throw 'Broker closed the connection' }
        $offset += $read
    }
    return $bytes
}

function Read-MqttPacket($Stream) {
    $packetType = (Read-Exact $Stream 1)[0]
    $remaining = 0
    $multiplier = 1
    do {
        $digit = (Read-Exact $Stream 1)[0]
        $remaining += ($digit -band 0x7f) * $multiplier
        $multiplier *= 128
        if ($multiplier -gt 268435456) { throw 'Malformed MQTT remaining length' }
    } while (($digit -band 0x80) -ne 0)
    $body = if ($remaining -gt 0) { Read-Exact $Stream $remaining } else { [byte[]]::new(0) }
    return @{ Type = $packetType; Body = $body }
}

function Open-MqttConnection([string]$HostName, [int]$Port, [string]$User, [string]$Password) {
    $client = [Net.Sockets.TcpClient]::new()
    $client.ReceiveTimeout = $TimeoutMs
    $client.SendTimeout = $TimeoutMs
    $client.Connect($HostName, $Port)
    $stream = $client.GetStream()

    $variable = [System.Collections.Generic.List[byte]]::new()
    Add-MqttString $variable 'MQTT'
    $variable.Add(4)
    $flags = 2
    if ($User) { $flags = $flags -bor 0x80 }
    if ($Password) { $flags = $flags -bor 0x40 }
    $variable.Add([byte]$flags)
    $variable.Add(0)
    $variable.Add(30)
    Add-MqttString $variable ("spectre-broker-probe-{0}" -f ([guid]::NewGuid().ToString('N').Substring(0, 8)))
    if ($User) { Add-MqttString $variable $User }
    if ($Password) { Add-MqttString $variable $Password }

    $packet = [System.Collections.Generic.List[byte]]::new()
    $packet.Add(0x10)
    $packet.AddRange([byte[]](ConvertTo-MqttRemainingLength $variable.Count))
    $packet.AddRange([byte[]]$variable.ToArray())
    $bytes = $packet.ToArray()
    $stream.Write($bytes, 0, $bytes.Length)
    $reply = Read-MqttPacket $stream
    if ($reply.Type -ne 0x20 -or $reply.Body.Length -ne 2 -or $reply.Body[1] -ne 0) {
        throw "MQTT CONNECT rejected (packet=0x$($reply.Type.ToString('x2')))"
    }
    return @{ Client = $client; Stream = $stream }
}

function Publish-Qos1($Stream, [string]$Topic, [byte[]]$Payload, [uint16]$PacketId) {
    $body = [System.Collections.Generic.List[byte]]::new()
    Add-MqttString $body $Topic
    $body.Add([byte](($PacketId -shr 8) -band 0xff))
    $body.Add([byte]($PacketId -band 0xff))
    $body.AddRange([byte[]]$Payload)
    $packet = [System.Collections.Generic.List[byte]]::new()
    $packet.Add(0x32)
    $packet.AddRange([byte[]](ConvertTo-MqttRemainingLength $body.Count))
    $packet.AddRange([byte[]]$body.ToArray())
    $bytes = $packet.ToArray()
    $Stream.Write($bytes, 0, $bytes.Length)
    $reply = Read-MqttPacket $Stream
    if ($reply.Type -ne 0x40 -or $reply.Body.Length -ne 2) { return $false }
    $acked = ([uint16]$reply.Body[0] -shl 8) -bor $reply.Body[1]
    return $acked -eq $PacketId
}

$hostName = Get-SecretDefine 'SPECTRE_MQTT_BROKER'
$port = [int](Get-SecretDefine 'SPECTRE_MQTT_PORT')
$user = Get-SecretDefine 'SPECTRE_MQTT_USER'
$password = Get-SecretDefine 'SPECTRE_MQTT_PASSWORD'
$sensorId = Get-SecretDefine 'SPECTRE_MQTT_SENSOR_ID'
$topicBase = Get-SecretDefine 'SPECTRE_MQTT_TOPIC_BASE'
$topic = "$topicBase/$sensorId/probe"

$results = foreach ($size in $PayloadSizes) {
    $connection = $null
    try {
        $connection = Open-MqttConnection $hostName $port $user $password
        $prefix = '{"type":"codex_mqtt_size_probe","diagnostic":true,"target_bytes":' + $size + ',"padding":"'
        $suffix = '"}'
        if ($prefix.Length + $suffix.Length -gt $size) { throw "Payload size $size is too small" }
        $payloadText = $prefix + ('x' * ($size - $prefix.Length - $suffix.Length)) + $suffix
        $payload = [Text.Encoding]::UTF8.GetBytes($payloadText)
        $started = Get-Date
        $acked = Publish-Qos1 $connection.Stream $topic $payload ([uint16](1000 + $size))
        [pscustomobject]@{
            PayloadBytes = $payload.Length
            PacketBytes = $payload.Length + [Text.Encoding]::UTF8.GetByteCount($topic) + 7
            Result = if ($acked) { 'PUBACK' } else { 'unexpected_reply' }
            ElapsedMs = [int]((Get-Date) - $started).TotalMilliseconds
        }
    } catch {
        [pscustomobject]@{
            PayloadBytes = $size
            PacketBytes = $size + [Text.Encoding]::UTF8.GetByteCount($topic) + 7
            Result = $_.Exception.Message
            ElapsedMs = $TimeoutMs
        }
    } finally {
        if ($connection -and $connection.Client) { $connection.Client.Dispose() }
    }
}

$results | Format-Table -AutoSize
