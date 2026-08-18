[CmdletBinding()]
param([switch]$NoLaunch, [switch]$DesktopShortcut)

$ErrorActionPreference = 'Stop'
$repo = 'Kwiatens/Inventatory-Software'
$installRoot = Join-Path $env:LOCALAPPDATA 'Programs\Inventatory'
$downloadRoot = Join-Path $env:TEMP ('Inventatory-' + [guid]::NewGuid())
$stagingRoot = "$installRoot.staging"
$backupRoot = "$installRoot.backup"

function New-Shortcut([string]$path, [string]$target, [string]$arguments = '') {
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut($path)
  $shortcut.TargetPath = $target
  $shortcut.Arguments = $arguments
  $shortcut.WorkingDirectory = Split-Path $target
  $shortcut.Save()
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
  Start-Process -FilePath $exe -WorkingDirectory $workingDirectory
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
  if (-not (Test-Path (Join-Path $packageRoot 'inventatory.exe'))) { throw 'Release archive is missing Inventatory.' }
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
  $programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
  New-Shortcut (Join-Path $programs 'Inventatory.lnk') $exe
  $makeDesktop = $DesktopShortcut
  if (-not $DesktopShortcut) { $makeDesktop = (Read-Host 'Create a desktop shortcut? [y/N]') -match '^[Yy]' }
  if ($makeDesktop) { New-Shortcut (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Inventatory.lnk') $exe }

  Write-Host "Inventatory $tag installed for this Windows user."
  if (-not $NoLaunch) { Start-InventatoryAfterCountdown $exe $installRoot }
} finally {
  Remove-Item -LiteralPath $downloadRoot -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
}
