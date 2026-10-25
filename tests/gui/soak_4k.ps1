param(
  [Parameter(Mandatory = $true)][string]$Video,
  [double]$Minutes = 60,
  [int]$SampleSeconds = 10,
  [string]$OutDir = "$env:TEMP\opencode\domino-soak"
)

# 4K 实时 SR 长稳：真实播放窗口 + 循环 + 周期 seek，记录进程/GPU 资源。
# 用法：
#   powershell -ExecutionPolicy Bypass -File tests/gui/soak_4k.ps1 -Video "D:\movie.mp4" -Minutes 60
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exe = Join-Path $repo 'flutter\build\windows\x64\runner\Debug\domino_flutter.exe'
$rclick = Join-Path $PSScriptRoot 'rclick.ps1'
$capture = Join-Path $PSScriptRoot 'capture.ps1'
if (!(Test-Path $exe)) { throw "app not built: $exe" }
if (!(Test-Path $Video)) { throw "video not found: $Video" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$csv = Join-Path $OutDir "soak-$stamp.csv"
$log = Join-Path $OutDir "soak-$stamp.log"
$png = Join-Path $OutDir "soak-$stamp.png"

function Log([string]$msg) {
  $line = "$(Get-Date -Format o) $msg"
  Add-Content -Path $log -Value $line
  Write-Output $line
}

function GpuTotalMb() {
  try { return (& nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>$null | Select-Object -First 1).Trim() }
  catch { return '' }
}

# 读取诊断 JSON：打开侧栏 -> 诊断 -> 复制 -> 关侧栏；用于确认本轮确实跑 4K 零拷贝。
function Get-Diagnostics([string]$windowHandle) {
  & $rclick -Hwnd $windowHandle -X 129 -Y 650 | Out-Null
  Start-Sleep -Milliseconds 500
  & $rclick -Hwnd $windowHandle -X 1214 -Y 104 | Out-Null
  Start-Sleep -Milliseconds 500
  & $rclick -Hwnd $windowHandle -X 1250 -Y 145 | Out-Null
  Start-Sleep -Milliseconds 300
  $json = Get-Clipboard -Raw
  & $rclick -Hwnd $windowHandle -X 400 -Y 400 | Out-Null
  Start-Sleep -Milliseconds 400
  return $json
}

Get-Process domino_flutter -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
# 4K soak 期间关闭自动降档，避免中途变成 1080p 后数据仍被当作 4K。
$env:DOMINO_NO_AUTO_DEGRADE = '1'
$err = Join-Path $OutDir "stderr-$stamp.log"
$p = Start-Process -FilePath $exe -RedirectStandardError $err -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
if ($p.HasExited) { throw "app exited at startup code=$($p.ExitCode)" }
$hwnd = $p.MainWindowHandle
if ($hwnd -eq 0) { throw 'app window not ready' }

# 空播放页中央「打开」按钮；失败时重试 4 次。
$tries = 0
while ($p.MainWindowHandle -eq $hwnd -and $tries -lt 4) {
  & $rclick -Hwnd $hwnd -X 640 -Y 395 | Out-Null
  Start-Sleep -Milliseconds 900
  $p.Refresh()
  $tries++
}
if ($p.MainWindowHandle -eq $hwnd) { throw 'file dialog did not open' }
Set-Clipboard -Value $Video
[System.Windows.Forms.SendKeys]::SendWait('%n')
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait('^a')
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait('^v')
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
Start-Sleep -Seconds 6
$p.Refresh()
if ($p.HasExited) { throw "app exited after open code=$($p.ExitCode)" }

# SR 4K + 循环；坐标基于 1280x720 客户区（窗口缩放 100%）。
& $rclick -Hwnd $hwnd -X 170 -Y 650 | Out-Null
Start-Sleep -Milliseconds 700
& $rclick -Hwnd $hwnd -X 183 -Y 645 | Out-Null
Start-Sleep -Seconds 8
& $rclick -Hwnd $hwnd -X 88 -Y 650 | Out-Null
Start-Sleep -Seconds 2
$p.Refresh()
if ($p.HasExited) { throw "app exited after SR/loop code=$($p.ExitCode)" }
# TensorRT 首次烘焙 engine 可能 12–20s；等 4K 零拷贝真正 active 再开始计时。
$diag = $null
for ($i = 0; $i -lt 8; $i++) {
  $diag = (Get-Diagnostics $hwnd) | ConvertFrom-Json
  if ($diag -and $diag.sr.targetW -ge 3840 -and $diag.zeroCopy.active) { break }
  Start-Sleep -Seconds 5
}
if (!$diag -or $diag.sr.targetW -lt 3840 -or !$diag.zeroCopy.active) {
  throw "soak did not reach 4K zero-copy: targetW=$($diag.sr.targetW) zeroCopy=$($diag.zeroCopy.active)"
}
Log "opened $Video; pid=$($p.Id); backend=$($diag.backend); targetW=$($diag.sr.targetW); zeroCopy=$($diag.zeroCopy.active); window='$($p.MainWindowTitle)'"

"time,elapsed_s,responding,working_mb,private_mb,handles,threads,cpu_s,gpu_total_mb" |
  Set-Content -Path $csv -Encoding UTF8

$start = Get-Date
$nextSeek = 600.0     # 每 10 分钟 seek 一次，覆盖 seek/清队列/重建 slot
$nextSample = 0.0
$duration = $Minutes * 60.0
while ($true) {
  $elapsed = ((Get-Date) - $start).TotalSeconds
  if ($elapsed -ge $duration) { break }
  $p.Refresh()
  if ($p.HasExited) { throw "app exited during soak code=$($p.ExitCode) at ${elapsed}s" }

  if ($elapsed -ge $nextSample) {
    $working = [math]::Round($p.WorkingSet64 / 1MB, 1)
    $private = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
    $gpuTotal = GpuTotalMb
    $cpu = [math]::Round($p.TotalProcessorTime.TotalSeconds, 1)
    $line = "{0:o},{1:F0},{2},{3},{4},{5},{6},{7},{8}" -f (Get-Date), $elapsed, $p.Responding,
      $working, $private, $p.HandleCount, $p.Threads.Count, $cpu, $gpuTotal
    Add-Content -Path $csv -Value $line
    $nextSample = $elapsed + $SampleSeconds
  }

  if ($elapsed -ge $nextSeek) {
    # 跳到当前进度 + 20 分钟，覆盖 seek/清队列/重建 slot。
    & $rclick -Hwnd $hwnd -X 700 -Y 650 | Out-Null
    Log "periodic seek at ${elapsed}s"
    $nextSeek += 600.0
  }
  Start-Sleep -Seconds 2
}

$diagEnd = (Get-Diagnostics $hwnd) | ConvertFrom-Json
if (!$diagEnd -or $diagEnd.sr.targetW -lt 3840 -or !$diagEnd.zeroCopy.active) {
  Log "WARNING: end state is not 4K zero-copy: targetW=$($diagEnd.sr.targetW) zeroCopy=$($diagEnd.zeroCopy.active); this run is invalid"
}
& $capture -Hwnd $hwnd -OutPng $png | Out-Null
Log "soak completed; csv=$csv; png=$png; targetW=$($diagEnd.sr.targetW); zeroCopy=$($diagEnd.zeroCopy.active); handles=$($p.HandleCount) threads=$($p.Threads.Count)"
if (!$p.HasExited) {
  $p.CloseMainWindow() | Out-Null
  Start-Sleep -Seconds 2
  if (!$p.HasExited) { $p.Kill() }
}
Remove-Item Env:DOMINO_NO_AUTO_DEGRADE -ErrorAction SilentlyContinue
