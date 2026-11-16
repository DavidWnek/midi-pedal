# Builds the MIDI Button firmware into build\midi-button-<board>.uf2.
# Needs the Arduino "Raspberry Pi Pico/RP2040/RP2350" core (earlephilhower) and, for
# boards with an RGB LED, the Adafruit NeoPixel library. arduino-cli ships with Arduino IDE 2.
#
#   .\build.ps1                          # Waveshare RP2350-Zero
#   .\build.ps1 -Board rpipico2          # Raspberry Pi Pico 2
#   .\build.ps1 -Upload -Port COM7       # build and flash over USB

param(
    [string]$Board = 'waveshare_rp2350_zero',
    [switch]$Upload,
    [string]$Port
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $cli) {
    $cli = Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
}
if (-not (Test-Path $cli)) { throw 'arduino-cli not found. Install Arduino IDE 2 or put arduino-cli on PATH.' }

$fqbn = "rp2040:rp2040:${Board}:usbstack=picosdk"
$sketch = Join-Path $PSScriptRoot 'MidiButton'
$work = Join-Path $env:TEMP "midi-button-build\$Board"
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

Write-Host "Building $fqbn"
& $cli compile --fqbn $fqbn --build-path $work $sketch
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$uf2 = Join-Path $out "midi-button-$Board.uf2"
Copy-Item (Join-Path $work 'MidiButton.ino.uf2') $uf2 -Force
Get-Item $uf2 | Select-Object Name, Length, LastWriteTime

if ($Upload) {
    if (-not $Port) { throw 'Pass -Port (the board''s COM port), or copy the .uf2 onto the RP2350 drive.' }
    & $cli upload --fqbn $fqbn --input-dir $work -p $Port $sketch
    if ($LASTEXITCODE -ne 0) { throw 'upload failed' }
}
