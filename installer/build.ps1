param(
  [string]$Version = "",
  [switch]$SkipBuild,
  [switch]$NoInstaller,
  [string]$OutputDir = ""
)

# 构建发布目录、绿色 zip 和 Inno Setup 安装包。
# 用法（仓库根目录）：
#   powershell -ExecutionPolicy Bypass -File installer/build.ps1
#   powershell -ExecutionPolicy Bypass -File installer/build.ps1 -SkipBuild -NoInstaller
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$flutter = Join-Path $repo 'flutter'
if (!$OutputDir) { $OutputDir = Join-Path $repo 'dist' }

if (!$Version) {
  $pubspec = Get-Content (Join-Path $flutter 'pubspec.yaml') -Raw
  if ($pubspec -notmatch '(?m)^version:\s*([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?)') {
    throw 'cannot read version from flutter/pubspec.yaml'
  }
  $Version = $Matches[1]
}
if (!(Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir | Out-Null }

if (!$SkipBuild) {
  Push-Location $flutter
  try {
    & flutter build windows --release
    if ($LASTEXITCODE -ne 0) { throw "flutter build failed: $LASTEXITCODE" }
  } finally { Pop-Location }
}

$release = Join-Path $flutter 'build\windows\x64\runner\Release'
if (!(Test-Path (Join-Path $release 'domino_flutter.exe'))) {
  throw "release runner not found: $release"
}

# cudart is optional; include it when present, otherwise app falls back to CPU.
$cudart = Join-Path $repo 'libs\cuda-runtime\bin\cudart64_12.dll'
$releaseCudart = Join-Path $release 'cudart64_12.dll'
if ((Test-Path $cudart) -and !(Test-Path $releaseCudart)) {
  Copy-Item $cudart $releaseCudart -Force
}

$stage = Join-Path $OutputDir "domino-player-v$Version-win64"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item (Join-Path $release '*') $stage -Recurse -Force
Copy-Item (Join-Path $repo 'LICENSE') $stage -Force
Copy-Item (Join-Path $repo 'THIRD_PARTY_NOTICES.md') $stage -Force
Copy-Item (Join-Path $PSScriptRoot 'USAGE.zh-CN.txt') (Join-Path $stage '使用说明.txt') -Force

$required = @(
  'domino_flutter.exe', 'domino_engine.dll', 'flutter_windows.dll',
  'data\app.so', 'assets\models\sr\ncnn\RealESRGAN_General_x4_v3.bin'
)
foreach ($rel in $required) {
  if (!(Test-Path (Join-Path $stage $rel))) { throw "release payload missing: $rel" }
}

$zip = Join-Path $OutputDir "domino-player-v$Version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Write-Output "creating $zip ..."
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash
"$zipHash  $(Split-Path $zip -Leaf)" | Set-Content "$zip.sha256" -Encoding ASCII

$setupHash = $null
if (!$NoInstaller) {
  $iscc = (Get-Command iscc.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
  if (!$iscc) {
    foreach ($p in @(
      (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
      (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
      (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    )) {
      if ($p -and (Test-Path $p)) { $iscc = $p; break }
    }
  }
  if (!$iscc) {
    Write-Warning 'ISCC.exe not found; set up Inno Setup 6 or pass -NoInstaller. zip is still available.'
  } else {
    Write-Output "building installer with $iscc ..."
    $generated = Join-Path $PSScriptRoot 'build.generated.iss'
    $escapedStage = $stage.Replace('\', '/')
    $escapedOut = $OutputDir.Replace('\', '/')
    @(
      "#define AppVersion `"$Version`""
      "#define SourceDir `"$escapedStage`""
      "#define OutputDir `"$escapedOut`""
    ) | Set-Content $generated -Encoding UTF8
    try {
      & $iscc (Join-Path $PSScriptRoot 'domino.iss')
      if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed: $LASTEXITCODE" }
    } finally {
      Remove-Item $generated -Force -ErrorAction SilentlyContinue
    }
    $setup = Join-Path $OutputDir "domino-player-v$Version-win64-setup.exe"
    if (!(Test-Path $setup)) { throw "installer not produced: $setup" }
    $setupHash = (Get-FileHash $setup -Algorithm SHA256).Hash
    "$setupHash  $(Split-Path $setup -Leaf)" | Set-Content "$setup.sha256" -Encoding ASCII
    Write-Output "installer: $setup"
  }
}

Write-Output "payload: $stage"
Write-Output "zip: $zip"
Write-Output "zip sha256: $zipHash"
if ($setupHash) { Write-Output "setup sha256: $setupHash" }
