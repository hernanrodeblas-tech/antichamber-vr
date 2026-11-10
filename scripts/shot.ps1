# Development aid: runs the game in fake-headset mode with optional Debug* options
# (written to the mod's ini, so Steam keeps running) and keeps the dumped eye images.
param(
    [string]$Node = '',
    [switch]$NoLinework,
    [int]$Frame = 1500,
    [string]$Tag = 'shot',
    [string]$Extra = '',
    [string]$Yaw = '',
    [string]$Pitch = '',
    [string]$Walk = ''
)
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$bin = "$root\Binaries\Win32"
$mod = "$root\VRMod"
$ini = "$bin\antichamber_vr.ini"
(Get-Content "$mod\config\antichamber_vr.ini") -replace '^FakeHMD=0', 'FakeHMD=1' -replace '^MaxRenderWidth=3000', 'MaxRenderWidth=1000' -replace '^DebugDumpFrame=0', "DebugDumpFrame=$Frame" | Set-Content $ini -Encoding ascii
$lines = @('DebugLog=1')
if ($Node) { $lines += "DebugJumpNode=$Node" }
if ($NoLinework) { $lines += 'DebugNoLinework=1' }
if ($Yaw) { $lines += "DebugFakeYaw=$Yaw" }
if ($Pitch) { $lines += "DebugFakePitch=$Pitch" }
if ($Walk) { $lines += "DebugWalkMs=$Walk" }
if ($Extra) { $lines += $Extra -split "`n" }
Add-Content $ini $lines
Remove-Item "$bin\vr_eye*.jpg" -ErrorAction SilentlyContinue
& "$mod\scripts\run_game.ps1" -Marker 'dumped eye 1|EXCEPTION|giving up|flat mode' -TimeoutSec 400 -Kill
foreach ($e in 0, 1) { if (Test-Path "$bin\vr_eye$e.jpg") { Copy-Item "$bin\vr_eye$e.jpg" "$mod\build\${Tag}_eye$e.jpg" -Force } }
Get-Content "$bin\antichamber_vr.log" | Select-String 'TakeDamage on|dumped|EXCEPTION|d3d9.dll\+' | ForEach-Object { $_.Line -replace '^\[\s*\d+\]\[t\s*(\d+)\] ', '' }
