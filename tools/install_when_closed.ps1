# deploy_when_closed.ps1 -- wait for the game to exit, deploy the new plugin, restart it.
# The plugin DLL is loaded by the running game, so it cannot be replaced while it runs.
param([int]$timeoutSec = 2400)

$src = 'D:\a11yb\a11y6.dll'
$dst = 'D:\Harness工作区\senren-banka-copy\plugin\a11y.dll'
$exe = 'D:\Harness工作区\senren-banka-copy\SenrenBanka.exe'
$diag = 'D:\a11yb\a11y5_diag.txt'

$deadline = (Get-Date).AddSeconds($timeoutSec)
Write-Output "waiting for SenrenBanka to exit (up to $timeoutSec s) ..."
while((Get-Process SenrenBanka -ErrorAction SilentlyContinue) -and ((Get-Date) -lt $deadline)){
  Start-Sleep -Seconds 2
}
if(Get-Process SenrenBanka -ErrorAction SilentlyContinue){
  Write-Output "TIMEOUT: game still running, nothing deployed"
  exit 1
}
Write-Output "game closed; copying plugin"

$ok = $false
for($i = 0; $i -lt 40; $i++){
  try { Copy-Item $src $dst -Force -ErrorAction Stop; $ok = $true; break }
  catch { Start-Sleep -Milliseconds 500 }
}
if(-not $ok){ Write-Output "COPY FAILED (file still locked?)"; exit 1 }
Write-Output ("deployed a11y.dll = " + (Get-Item $dst).Length + " bytes")

# mark the diag so we can tell a fresh run apart
Add-Content -Path $diag -Value "`n=== restart for new build $(Get-Date -Format 'HH:mm:ss') ===" -Encoding UTF8

Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
Write-Output "game restarted"

Start-Sleep -Seconds 25
if(Test-Path $diag){
  Write-Output "--- diag tail ---"
  Get-Content $diag -Tail 12 -Encoding UTF8
}
