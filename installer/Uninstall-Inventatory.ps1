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
  $currentSession = (Get-Process -Id $PID).SessionId
  foreach ($candidate in $candidates) {
    if ([string]::IsNullOrWhiteSpace($candidate.ExecutablePath)) {
      # Another user's process cannot be inspected without elevation; it runs in its own session, so
      # it cannot be the copy that this script replaces.
      if ([int]$candidate.SessionId -ne [int]$currentSession) { continue }
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

# The installer leaves staging, backup and failed-rollback copies next to the install folder.
function Remove-InventatoryInstallLeftovers {
  $installParent = Split-Path -Parent $installRoot
  if (-not (Test-Path -LiteralPath $installParent)) { return }
  Get-ChildItem -LiteralPath $installParent -Directory -Filter 'Inventatory.*' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^Inventatory\.(staging|backup\..+|failed\..+)$' } |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
}

# Credential Manager holds the DigiKey secret and the Scan R1 pairing token under 'Inventatory/<name>'
# targets. They belong to the data that the user chose to delete, so they are removed with it.
# Anything that could not be deleted is collected here and listed at the end instead of being reported as removed.
$script:leftBehind = New-Object System.Collections.Generic.List[string]

function Remove-InventatoryStoredSecrets {
  $listing = @(& cmdkey.exe /list 2>$null)
  foreach ($line in $listing) {
    if ([string]$line -match '(Inventatory/\S+)') {
      $target = $Matches[1]
      & cmdkey.exe "/delete:$target" 2>$null | Out-Null
      if ($LASTEXITCODE -ne 0) { $script:leftBehind.Add("Credential Manager entry $target") }
    }
  }
}

function Remove-InventatoryUserFolder([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return }
  try {
    Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop
  } catch {
    $script:leftBehind.Add($path)
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
Remove-InventatoryInstallLeftovers
if ((Read-Host 'Also delete Documents\Inventatory and local settings? [y/N]') -match '^[Yy]') {
  # MyDocuments follows a redirected Documents folder (for example OneDrive known-folder backup).
  Remove-InventatoryUserFolder (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Inventatory')
  Remove-InventatoryUserFolder (Join-Path $env:LOCALAPPDATA 'Inventatory')
  Remove-InventatoryStoredSecrets
}
if ($script:leftBehind.Count -gt 0) {
  Write-Warning 'Inventatory was removed, but these items could not be deleted. Remove them manually:'
  foreach ($item in $script:leftBehind) { Write-Warning "  $item" }
  exit 1
}
Write-Host 'Inventatory was removed.'
