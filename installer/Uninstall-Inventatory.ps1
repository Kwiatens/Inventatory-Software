$ErrorActionPreference = 'Stop'
$installRoot = Join-Path $env:LOCALAPPDATA 'Programs\Inventatory'
$bootstrapDownloadRoot = Join-Path $env:TEMP 'Inventatory-install'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

function ConvertTo-NormalizedPath([string]$path) {
  return [System.IO.Path]::GetFullPath($path).TrimEnd([System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar)
}

function Get-InventatoryProcesses {
  $expectedPaths = @(
    (ConvertTo-NormalizedPath (Join-Path $installRoot 'inventatory.exe')),
    (ConvertTo-NormalizedPath (Join-Path $installRoot 'inventatory-background.exe'))
  )
  $candidates = @(Get-CimInstance -ClassName Win32_Process `
    -Filter "Name = 'inventatory.exe' OR Name = 'inventatory-background.exe'" -ErrorAction Stop)
  foreach ($candidate in $candidates) {
    if ([string]::IsNullOrWhiteSpace($candidate.ExecutablePath)) {
      throw "Unable to verify the executable path for Inventatory process $($candidate.ProcessId). Close Inventatory and run the uninstaller again."
    }
    $candidatePath = ConvertTo-NormalizedPath $candidate.ExecutablePath
    if ($expectedPaths -contains $candidatePath) {
      [PSCustomObject]@{
        Id = [int]$candidate.ProcessId
        Name = [string]$candidate.Name
        Path = $candidatePath
      }
    }
  }
}

function Assert-InventatoryProcessesStopped {
  $running = @(Get-InventatoryProcesses)
  if ($running.Count -ne 0) {
    $description = ($running | ForEach-Object { "$($_.Name) (PID $($_.Id))" }) -join ', '
    throw "Inventatory is still running: $description. Close all Inventatory windows and tray services, then run the uninstaller again."
  }
}

function Remove-InventatoryStartupRegistration {
  if (-not (Test-Path -LiteralPath $runKey)) { return }
  foreach ($valueName in @('Inventatory Background Service', 'InventatorySoftware', 'HIMSSoftware')) {
    if ($null -ne (Get-ItemProperty -LiteralPath $runKey -Name $valueName -ErrorAction SilentlyContinue)) {
      Remove-ItemProperty -LiteralPath $runKey -Name $valueName -ErrorAction Stop
    }
  }
}

Assert-InventatoryProcessesStopped
Remove-InventatoryStartupRegistration
# Removing the startup entry can race with Windows launching the background
# process. Re-check before deleting the installation so a running process never
# loses its files or an in-progress database write.
Assert-InventatoryProcessesStopped
if (Test-Path -LiteralPath $installRoot) {
  Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction Stop
}
Remove-Item -LiteralPath (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Inventatory.lnk') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Inventatory.lnk') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $bootstrapDownloadRoot -Recurse -Force -ErrorAction SilentlyContinue
if ((Read-Host 'Also delete Documents\Inventatory and local settings? [y/N]') -match '^[Yy]') {
  Remove-Item -LiteralPath (Join-Path $env:USERPROFILE 'Documents\Inventatory') -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath (Join-Path $env:LOCALAPPDATA 'Inventatory') -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'Inventatory was removed.'
