param(
  [switch]$WindowsTerminal
)

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

function Get-WindowsTerminalSettingsPath {
  $paths = @()
  if ($env:LOCALAPPDATA) {
    $paths += Join-Path $env:LOCALAPPDATA 'Microsoft\Windows Terminal\settings.json'
    $packagesRoot = Join-Path $env:LOCALAPPDATA 'Packages'
    if (Test-Path -LiteralPath $packagesRoot) {
      $paths += Get-ChildItem -LiteralPath $packagesRoot -Directory -Filter 'Microsoft.WindowsTerminal*' -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'LocalState\settings.json' }
    }
  }
  return $paths | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique
}

function Test-WindowsTerminalProfile {
  param([string]$Name)

  foreach ($settingsPath in Get-WindowsTerminalSettingsPath) {
    try {
      $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
      foreach ($profile in @($settings.profiles.list)) {
        if ($profile.name -eq $Name) {
          return $true
        }
      }
    } catch {
      # An unreadable or partially edited settings file should not block the
      # ordinary console fallback.
    }
  }
  return $false
}

if ($WindowsTerminal) {
  $wt = Get-Command wt.exe -ErrorAction SilentlyContinue
  if ($wt -and (Test-WindowsTerminalProfile -Name 'Inventatory')) {
    $wtArguments = @(
      '-w', '0',
      'new-tab',
      '--profile', 'Inventatory',
      '--title', 'Inventatory',
      '--startingDirectory', (Split-Path $exe),
      $exe
    )
    & $wt.Source @wtArguments
    if ($LASTEXITCODE -eq 0) {
      exit 0
    }
    Write-Warning 'Windows Terminal could not open the Inventatory profile; using the normal console.'
  } else {
    Write-Warning 'Windows Terminal or the Inventatory profile was not found; using the normal console.'
  }
}

Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Normal

