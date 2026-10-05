$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$out = Join-Path $root "build\test-game-mode-cycle-latch"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$compiler = Get-Command clang++ -ErrorAction SilentlyContinue
if (-not $compiler) {
    $compiler = Get-Command g++ -ErrorAction Stop
}

& $compiler.Source -std=c++20 -Wall -Wextra -Werror `
    -I (Join-Path $root "src") `
    (Join-Path $PSScriptRoot "game_mode_cycle_latch_test.cpp") `
    -o (Join-Path $out "game_mode_cycle_latch_test.exe")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $out "game_mode_cycle_latch_test.exe")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Output "game-mode carousel: one cycle per press, repeats ignored, release rearms: ok"
