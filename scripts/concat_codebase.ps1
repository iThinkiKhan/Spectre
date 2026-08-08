param(
  [string]$RepoRoot = (Get-Location).Path,
  [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path $RepoRoot).Path
if (-not (Test-Path (Join-Path $repo '.git'))) {
  throw "RepoRoot must point at the Spectre repo root: $RepoRoot"
}

if ([string]::IsNullOrWhiteSpace($OutFile)) {
  $stamp = Get-Date -Format 'yyyy-MM-dd'
  $OutFile = Join-Path $repo "spectre_full_${stamp}_ai_upload.txt"
} elseif (-not [System.IO.Path]::IsPathRooted($OutFile)) {
  $OutFile = Join-Path $repo $OutFile
}

$allowedExt = @(
  '.c','.cc','.cpp','.h','.hpp','.ino','.py','.js','.mjs','.ts','.tsx',
  '.json','.md','.ini','.yaml','.yml','.xml','.kts','.kt','.gradle',
  '.properties','.ps1','.bat','.sh','.csv','.html','.css','.scss','.sql',
  '.toml','.rb','.dart','.java'
)

$allowedBase = @(
  'gradlew','LICENSE','Makefile','Dockerfile','.gitignore','.clang-format',
  '.editorconfig','platformio.ini','mkdocs.yml','CMakeLists.txt','README',
  'README.md','app.json','package.json','package-lock.json'
)

function Should-Skip([string]$rel) {
  $n = $rel.Replace('\','/')

  if ($n -eq '.claude/settings.local.json') { return $true }
  if ($n -eq 'companion_app/android/local.properties') { return $true }
  if ($n -eq 'src/secrets.h') { return $true }
  if ($n -eq 'companion_app/android/app/src/main/java/com/spectre/companion/SpectreSecrets.kt') { return $true }
  if ($n -eq 'nonascii.txt') { return $true }
  if ($n -eq 'spectrelatestlog') { return $true }
  if ($n.StartsWith('spectre_full_') -and $n.EndsWith('.txt')) { return $true }
  if ($n.StartsWith('.claude/worktrees/')) { return $true }
  if ($n.StartsWith('.cache/')) { return $true }
  if ($n.StartsWith('.pio/')) { return $true }
  if ($n.StartsWith('companion_app/android/.gradle/')) { return $true }
  if ($n.StartsWith('companion_app/android/app/build/')) { return $true }
  if ($n.StartsWith('companion_app/android/build/')) { return $true }
  if ($n.StartsWith('companion-app-scaffold/.idea/')) { return $true }
  if ($n.StartsWith('diagnostics/')) {
    return -not $n.EndsWith('.ps1')
  }

  return $n -match '\.(log(\..+)?|hprof|png|jpe?g|gif|webp|jar|apk|dex|so|a|class|exe|zip)$'
}

function Is-TextCandidate([string]$rel) {
  $base = Split-Path $rel -Leaf
  $ext = [System.IO.Path]::GetExtension($rel).ToLowerInvariant()
  return ($allowedBase -contains $base) -or ($allowedExt -contains $ext) -or ($base -eq 'gradlew')
}

$tracked = & git -C $repo ls-files -co --exclude-standard
$selected = foreach ($rel in $tracked) {
  if ([string]::IsNullOrWhiteSpace($rel)) { continue }
  if (Should-Skip $rel) { continue }
  if (-not (Is-TextCandidate $rel)) { continue }

  $full = Join-Path $repo $rel
  try {
    $bytes = [System.IO.File]::ReadAllBytes($full)
    if ($bytes -contains 0) { continue }
    $rel
  } catch {
    continue
  }
}

$selected = $selected | Sort-Object

$generated = Get-Date -Format 'yyyy-MM-dd HH:mm:ss K'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('# Spectre Full Code Backup')
[void]$sb.AppendLine("# Generated: $generated")
[void]$sb.AppendLine('# Root: C:\PlatformIO\Projects\Spectre')
[void]$sb.AppendLine("# File count: $($selected.Count)")
[void]$sb.AppendLine()

foreach ($rel in $selected) {
  $full = Join-Path $repo $rel
  $content = [System.IO.File]::ReadAllText($full)
  [void]$sb.AppendLine("===== BEGIN FILE: $rel =====")
  [void]$sb.AppendLine()
  [void]$sb.Append($content)
  if (-not $content.EndsWith("`n")) {
    [void]$sb.AppendLine()
  }
  [void]$sb.AppendLine("===== END FILE: $rel =====")
  [void]$sb.AppendLine()
}

[System.IO.File]::WriteAllText($OutFile, $sb.ToString())
Write-Host "Wrote $OutFile"
Write-Host "Files: $($selected.Count)"
