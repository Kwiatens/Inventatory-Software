$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

cmake -S . -B build
cmake --build build

$exeCandidates = @(
  Join-Path $root 'build\Debug\inventatory.exe'
  Join-Path $root 'build\Release\inventatory.exe'
)

$exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) {
  throw 'Could not find inventatory.exe after building.'
}

Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Normal

