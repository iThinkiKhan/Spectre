param(
  [string]$DestinationRoot = ""
)

$ErrorActionPreference = "Stop"

$appRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")
if (-not $DestinationRoot) {
  $DestinationRoot = Join-Path (Resolve-Path -LiteralPath (Join-Path $appRoot.Path "..")) "backups"
}
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$destination = Join-Path $DestinationRoot "spectre-companion-$stamp.zip"

New-Item -ItemType Directory -Force -Path $DestinationRoot | Out-Null

$excludedFragments = @(
  "\.git\",
  "\node_modules\",
  "\.gradle-home\",
  "\android\.gradle\",
  "\android\build\",
  "\android\app\build\",
  "\android\app\.cxx\",
  "\.bundle\",
  "\tmp_bundle\",
  "\ios\Pods\"
)

$files =
  Get-ChildItem -LiteralPath $appRoot.Path -Recurse -File -Force |
  Where-Object {
    $path = $_.FullName
    -not ($excludedFragments | Where-Object { $path.Contains($_) })
  }

if (-not $files) {
  throw "No source files found to archive."
}

$files | Compress-Archive -DestinationPath $destination -Force
Write-Output $destination
