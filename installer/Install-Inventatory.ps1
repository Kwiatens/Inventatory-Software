[CmdletBinding()]
param([switch]$NoLaunch, [switch]$DesktopShortcut,
      [string]$Repository = '__INVENTATORY_RELEASE_REPOSITORY__')

$ErrorActionPreference = 'Stop'
if ($Repository -eq '__INVENTATORY_RELEASE_REPOSITORY__' -or
    $Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
  throw 'This installer must be downloaded from an official Inventatory release.'
}
$repo = $Repository
$installRoot = Join-Path $env:LOCALAPPDATA 'Programs\Inventatory'
$downloadRoot = Join-Path $env:TEMP ('Inventatory-' + [guid]::NewGuid())
$stagingRoot = "$installRoot.staging"
$backupRoot = "$installRoot.backup"

function New-Shortcut([string]$path, [string]$target, [string]$arguments = '', [string]$workingDirectory = '') {
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut($path)
  $shortcut.TargetPath = $target
  $shortcut.Arguments = $arguments
  $shortcut.WorkingDirectory = if ($workingDirectory) { $workingDirectory } else { Split-Path $target }
  $shortcut.Save()
}

function Start-InventatoryClassicConsole([string]$exe, [string]$workingDirectory) {
  # Launch conhost explicitly so installed shortcuts use the classic console
  # even when Windows Terminal is the system default for console applications.
  $consoleHost = Join-Path $env:SystemRoot 'System32\conhost.exe'
  $commandLine = 'title Inventatory && "{0}"' -f $exe
  Start-Process -FilePath $consoleHost `
    -WorkingDirectory $workingDirectory `
    -ArgumentList @($env:ComSpec, '/k', $commandLine)
}

function Start-InventatoryAfterCountdown([string]$exe, [string]$workingDirectory) {
  for ($remaining = 3; $remaining -gt 0; $remaining--) {
    Write-Host -NoNewline ("`rLaunching Inventatory in {0}... Press any key to cancel. " -f $remaining)
    $deadline = (Get-Date).AddSeconds(1)
    do {
      $keyAvailable = $false
      try { $keyAvailable = [Console]::KeyAvailable } catch { }
      if ($keyAvailable) {
        [Console]::ReadKey($true) | Out-Null
        Write-Host "`rInventatory launch cancelled. Run it later from the Start menu.        "
        return
      }
      Start-Sleep -Milliseconds 50
    } while ((Get-Date) -lt $deadline)
  }
  Write-Host "`rLaunching Inventatory now.                                      "
  Start-InventatoryClassicConsole $exe $workingDirectory
}

try {
  New-Item -ItemType Directory -Path $downloadRoot | Out-Null
  $releaseBase = "https://github.com/$repo/releases/latest/download"
  Invoke-WebRequest -UseBasicParsing -Uri "$releaseBase/Inventatory-win-x64.zip" -OutFile (Join-Path $downloadRoot 'Inventatory-win-x64.zip')
  Invoke-WebRequest -UseBasicParsing -Uri "$releaseBase/SHA256SUMS.txt" -OutFile (Join-Path $downloadRoot 'SHA256SUMS.txt')
  $tag = 'latest public beta'

  $archive = Join-Path $downloadRoot 'Inventatory-win-x64.zip'
  $checksums = Join-Path $downloadRoot 'SHA256SUMS.txt'
  $expected = ((Get-Content $checksums | Where-Object { $_ -match 'Inventatory-win-x64.zip$' } | Select-Object -First 1) -split '\s+')[0]
  $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
  if (-not $expected -or $actual -ne $expected.ToLowerInvariant()) { throw 'Release checksum verification failed. The existing installation was left unchanged.' }

  if (Get-Process inventatory -ErrorAction SilentlyContinue) {
    $answer = Read-Host 'Inventatory is running. Close it, then press Enter to continue (or type N to cancel)'
    if ($answer -match '^[Nn]') { throw 'Installation cancelled.' }
    if (Get-Process inventatory -ErrorAction SilentlyContinue) { throw 'Inventatory is still running. Close it and run the installer again.' }
  }

  Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
  Expand-Archive -LiteralPath $archive -DestinationPath $stagingRoot -Force
  $packageRoot = Join-Path $stagingRoot 'Inventatory'
  if (-not (Test-Path (Join-Path $packageRoot 'inventatory.exe')) -or
      -not (Test-Path (Join-Path $packageRoot 'inventatory-background.exe'))) {
    throw 'Release archive is missing Inventatory or its background launcher.'
  }
  Remove-Item -LiteralPath $backupRoot -Recurse -Force -ErrorAction SilentlyContinue
  if (Test-Path $installRoot) { Move-Item -LiteralPath $installRoot -Destination $backupRoot }
  try {
    Move-Item -LiteralPath $packageRoot -Destination $installRoot
    Remove-Item -LiteralPath $backupRoot -Recurse -Force -ErrorAction SilentlyContinue
  } catch {
    if (Test-Path $backupRoot) { Move-Item -LiteralPath $backupRoot -Destination $installRoot }
    throw
  }

  $exe = Join-Path $installRoot 'inventatory.exe'
  $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
  $legacyStartupNames = @('InventatorySoftware', 'HIMSSoftware')
  $hasLegacyStartup = $false
  if (Test-Path $runKey) {
    foreach ($legacyStartupName in $legacyStartupNames) {
      if ($null -ne (Get-ItemProperty -Path $runKey -Name $legacyStartupName -ErrorAction SilentlyContinue)) {
        $hasLegacyStartup = $true
        break
      }
    }
  }
  if ($hasLegacyStartup) {
    $launcher = Join-Path $installRoot 'inventatory-background.exe'
    Set-ItemProperty -Path $runKey -Name 'Inventatory Background Service' -Value ('"{0}" --background' -f $launcher)
    foreach ($legacyStartupName in $legacyStartupNames) {
      Remove-ItemProperty -Path $runKey -Name $legacyStartupName -ErrorAction SilentlyContinue
    }
  }
  $consoleHost = Join-Path $env:SystemRoot 'System32\conhost.exe'
  $consoleArguments = '"{0}" /k title Inventatory && "{1}"' -f $env:ComSpec, $exe
  $programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
  New-Shortcut (Join-Path $programs 'Inventatory.lnk') $consoleHost $consoleArguments $installRoot
  $makeDesktop = $DesktopShortcut
  if (-not $DesktopShortcut) { $makeDesktop = (Read-Host 'Create a desktop shortcut? [y/N]') -match '^[Yy]' }
  if ($makeDesktop) { New-Shortcut (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Inventatory.lnk') $consoleHost $consoleArguments $installRoot }

  Write-Host "Inventatory $tag installed for this Windows user."
  if (-not $NoLaunch) { Start-InventatoryAfterCountdown $exe $installRoot }
} finally {
  Remove-Item -LiteralPath $downloadRoot -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
}
