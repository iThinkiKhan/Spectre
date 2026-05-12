


param(
    [string]$ProjectRoot = ".",
    [string]$Environment = "esp32-s3-devkitc1-n16r8",
    [int]$Jobs = 1
)

$ErrorActionPreference = "Stop"
Set-Location $ProjectRoot

$pio = "C:\Users\JimSchneider\.platformio\penv\Scripts\platformio.exe"
if (-not (Test-Path $pio)) {
    throw "PlatformIO not found at $pio"
}

$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$freshBuildDir = "C:/PlatformIO/_build/Spectre_$ts"
$tmpIni = ".pio_temp_$ts.ini"

$ini = Get-Content -Path "platformio.ini" -Raw
if ($ini -match "(?ms)^\[platformio\].*?(?=^\[|\z)") {
    $ini = [regex]::Replace(
        $ini,
        "(?m)^build_dir\s*=.*$",
        "build_dir = $freshBuildDir"
    )
    if ($ini -notmatch "(?m)^build_dir\s*=") {
        $ini = [regex]::Replace(
            $ini,
            "(?m)^\[platformio\]\s*$",
            "[platformio]`r`nbuild_dir = $freshBuildDir"
        )
    }
} else {
    $ini = "[platformio]`r`nbuild_dir = $freshBuildDir`r`n`r`n$ini"
}

Set-Content -Path $tmpIni -Value $ini

Write-Host "Using fresh build dir: $freshBuildDir"
& $pio run -c $tmpIni -e $Environment -j $Jobs
$exitCode = $LASTEXITCODE

Remove-Item -Force $tmpIni -ErrorAction SilentlyContinue
exit $exitCode




