# Launches Antichamber through Steam (windowed) and waits until the mod log
# contains a marker, the game exits, or the timeout elapses.
param(
    [string]$Marker = 'ue::Init',
    [int]$TimeoutSec = 150,
    [switch]$Kill
)
$bin = (Resolve-Path "$PSScriptRoot\..\..\Binaries\Win32").Path
$log = "$bin\antichamber_vr.log"
Stop-Process -Name UDK -Force -ErrorAction SilentlyContinue
Start-Sleep 1
Remove-Item $log -ErrorAction SilentlyContinue
Start-Process "C:\Program Files (x86)\Steam\steam.exe" -ArgumentList "-applaunch", "219890", "-windowed", "-ResX=1280", "-ResY=720", "-forcelogflush"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$seen = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep 3
    $u = Get-Process UDK -ErrorAction SilentlyContinue
    if ($u) { $seen = $true }
    if ($seen -and -not $u) { "game exited"; break }
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $Marker -Quiet)) { "marker found"; break }
}
$u = Get-Process UDK -ErrorAction SilentlyContinue
"running=$([bool]$u)"
if ($Kill -and $u) { Stop-Process -Name UDK -Force }
