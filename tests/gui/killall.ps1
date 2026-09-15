param([string]$Name = "domino_p1")
$killed = @()
Get-Process $Name -ErrorAction SilentlyContinue | ForEach-Object {
  Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
  $killed += $_.Id
}
Start-Sleep -Milliseconds 400
$left = Get-Process $Name -ErrorAction SilentlyContinue
if ($left) {
  $left | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
  Write-Output ("[cleanup] killed {0}; left={1}" -f ($killed.Count), $left.Count)
} else {
  Write-Output "[cleanup] ok, no $Name running"
}
