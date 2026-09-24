[CmdletBinding()]
param([switch]$NoLaunch, [switch]$DesktopShortcut,
      [string]$Repository = '__INVENTATORY_RELEASE_REPOSITORY__',
      [string]$ReleaseTag = '',
      [switch]$UpdateMode, [string]$ArchivePath = '', [string]$ChecksumsPath = '',
      [string]$ReleaseVersion = '', [string]$CompletionPath = '', [string]$NotesPath = '',
      [int]$ParentProcessId = 0, [switch]$TestMode, [string]$TestInstallRoot = '',
      [switch]$TestActivationFailure, [string]$TestLaunchMarker = '')

$ErrorActionPreference = 'Stop'
$officialRepository = 'Kwiatens/Inventatory-Software'
$repo = $Repository
$installRoot = if ($TestMode -and $TestInstallRoot) { [System.IO.Path]::GetFullPath($TestInstallRoot) } else {
  Join-Path $env:LOCALAPPDATA 'Programs\Inventatory'
}
$downloadRoot = if ($UpdateMode) { $null } else { Join-Path $env:TEMP ('Inventatory-' + [guid]::NewGuid()) }
$stagingRoot = "$installRoot.staging"
$backupRoot = "$installRoot.backup." + [guid]::NewGuid().ToString('N')
$updatePackageRoot = $null
if ($TestMode -and (-not $UpdateMode -or -not $TestInstallRoot)) {
  throw 'Test mode requires update mode and a test installation root.'
}

function Write-UpdateStatus([string]$state, [string]$version, [string]$errorText = '') {
  if (-not $CompletionPath) { return }
  $parent = Split-Path -Parent $CompletionPath
  New-Item -ItemType Directory -Path $parent -Force | Out-Null
  $safeError = ($errorText -replace '[\r\n]+', ' ').Trim()
  if ($safeError.Length -gt 512) { $safeError = $safeError.Substring(0, 512) }
  $contents = @(
    'schema_version=1'
    ('state=' + $state)
    ('version=' + $version)
    ('error=' + $safeError)
  ) -join "`n"
  $temporary = "$CompletionPath.tmp-$([guid]::NewGuid().ToString('N'))"
  $backup = "$CompletionPath.backup-$([guid]::NewGuid().ToString('N'))"
  try {
    [System.IO.File]::WriteAllText($temporary, $contents + "`n", [System.Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $CompletionPath -PathType Leaf) {
      [System.IO.File]::Replace($temporary, $CompletionPath, $backup, $true)
    } else {
      [System.IO.File]::Move($temporary, $CompletionPath)
    }
  } finally {
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
  }
}

function Get-ExpectedChecksum([string]$manifestPath, [string]$assetName) {
  $matches = @(Get-Content -LiteralPath $manifestPath | Where-Object {
    $fields = $_ -split '\s+'
    $fields.Count -eq 2 -and $fields[0] -match '^[0-9A-Fa-f]{64}$' -and
      ($fields[1].TrimStart('*')) -eq $assetName
  })
  if ($matches.Count -ne 1) { throw "Checksum manifest does not contain exactly one entry for $assetName." }
  return (($matches[0] -split '\s+')[0]).ToLowerInvariant()
}

function Verify-UpdateAsset([string]$path, [string]$manifestPath, [string]$assetName) {
  $expectedHash = Get-ExpectedChecksum $manifestPath $assetName
  $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
  if ($actualHash -ne $expectedHash) { throw "Checksum verification failed for $assetName." }
}

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
      throw "Unable to verify the executable path for Inventatory process $($candidate.ProcessId). Close Inventatory and run the installer again."
    }
    $candidatePath = ConvertTo-NormalizedPath $candidate.ExecutablePath
    if ($expectedPaths -contains $candidatePath) {
      [PSCustomObject]@{
        Id = [int]$candidate.ProcessId
        Name = [string]$candidate.Name
        Path = $candidatePath
        CommandLine = [string]$candidate.CommandLine
      }
    }
  }
}

function Wait-InventatoryProcessesStopped([int]$timeoutSeconds = 30) {
  $deadline = [DateTime]::UtcNow.AddSeconds($timeoutSeconds)
  while ($true) {
    $running = @(Get-InventatoryProcesses)
    if ($running.Count -eq 0) {
      # Give Windows a short interval to release the image section after the
      # process exits before the directory swap begins.
      Start-Sleep -Milliseconds 100
      if (@(Get-InventatoryProcesses).Count -eq 0) { return }
    }
    if ([DateTime]::UtcNow -ge $deadline) {
      $description = ($running | ForEach-Object { "$($_.Name) (PID $($_.Id))" }) -join ', '
      throw "Inventatory is still running: $description. Close all Inventatory windows and tray services, then run the installer again. The existing installation was left unchanged."
    }
    Start-Sleep -Milliseconds 250
  }
}

function Quiesce-InventatoryProcesses {
  # Do not terminate the application: it owns the database and must be closed
  # through its normal UI/tray path so its final save can complete safely.
  $running = @(Get-InventatoryProcesses)
  if ($running.Count -eq 0) { return }
  $description = ($running | ForEach-Object { "$($_.Name) (PID $($_.Id))" }) -join ', '
  Write-Warning "Inventatory processes are running: $description. Close the foreground application and the Scan R1 tray service before continuing."
  $answer = Read-Host 'Press Enter after both are closed, or type N to cancel'
  if ($answer -match '^[Nn]') { throw 'Installation cancelled. The existing installation was left unchanged.' }
  Wait-InventatoryProcessesStopped
}

function New-Shortcut([string]$path, [string]$target, [string]$arguments = '', [string]$workingDirectory = '') {
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut($path)
  $shortcut.TargetPath = $target
  $shortcut.Arguments = $arguments
  $shortcut.WorkingDirectory = if ($workingDirectory) { $workingDirectory } else { Split-Path $target }
  $shortcut.Save()
}

function Get-InventatoryWindowsTerminal {
  $command = Get-Command wt.exe -ErrorAction SilentlyContinue
  if ($command) { return $command.Source }
  try {
    $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\wt.exe'
    if (Test-Path -LiteralPath $alias -PathType Leaf -ErrorAction SilentlyContinue) { return $alias }
  } catch { }
  return $null
}

function Start-InventatoryTerminal([string]$exe, [string]$workingDirectory) {
  if ($TestMode) {
    if ($TestLaunchMarker) { Set-Content -LiteralPath $TestLaunchMarker -Value $exe -NoNewline }
    return
  }
  $windowsTerminal = Get-InventatoryWindowsTerminal
  if ($windowsTerminal) {
    try {
      $arguments = @(
        'new-tab',
        '--title', 'Inventatory',
        '--startingDirectory', ('"{0}"' -f $workingDirectory),
        ('"{0}"' -f $exe)
      )
      Start-Process -FilePath $windowsTerminal -WorkingDirectory $workingDirectory -ArgumentList $arguments -ErrorAction Stop
      return
    } catch {
      # Fall through to the system default terminal if the Windows Terminal
      # command is registered but cannot open a tab.
    }
  }
  # Starting the executable directly lets Windows choose its configured
  # terminal application, while still working on systems without Windows
  # Terminal through the normal console-host fallback.
  Start-Process -FilePath $exe -WorkingDirectory $workingDirectory -WindowStyle Normal
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
  Start-InventatoryTerminal $exe $workingDirectory
}

$updateActivationCompleted = $false
$oldInstallExe = Join-Path $installRoot 'inventatory.exe'
try {
  if ($Repository -eq '__INVENTATORY_RELEASE_REPOSITORY__' -or
      $Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or
      $Repository -ine $officialRepository) {
    throw 'This installer must be downloaded from an official Inventatory release.'
  }
  if ($ReleaseTag -and
      $ReleaseTag -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$') {
    throw 'The installer release tag is invalid.'
  }
  if ($UpdateMode) {
    if (-not $ArchivePath -or -not $ChecksumsPath -or -not $ReleaseVersion -or -not $CompletionPath -or
        -not (Test-Path -LiteralPath $ArchivePath) -or -not (Test-Path -LiteralPath $ChecksumsPath) -or
        -not (Test-Path -LiteralPath $PSCommandPath)) {
      throw 'The predownloaded update package is incomplete.'
    }
    if ($ReleaseVersion -notmatch '^v[0-9]+(\.[0-9]+){1,3}(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$') { throw 'The update release version is invalid.' }
    $archive = (ConvertTo-NormalizedPath $ArchivePath)
    $checksums = (ConvertTo-NormalizedPath $ChecksumsPath)
    $updatePackageRoot = ConvertTo-NormalizedPath (Split-Path -Parent $archive)
    $tempRoot = ConvertTo-NormalizedPath ([System.IO.Path]::GetTempPath())
    $workspaceName = Split-Path -Leaf $updatePackageRoot
    if (-not $workspaceName.StartsWith('Inventatory-update-', [System.StringComparison]::OrdinalIgnoreCase) -or
        -not $updatePackageRoot.StartsWith($tempRoot + [System.IO.Path]::DirectorySeparatorChar,
                                           [System.StringComparison]::OrdinalIgnoreCase)) {
      throw 'The update package must be inside an Inventatory temporary update folder.'
    }
    Verify-UpdateAsset $archive $checksums 'Inventatory-win-x64.zip'
    Verify-UpdateAsset $PSCommandPath $checksums 'Install-Inventatory.ps1'
    # The foreground process intentionally remains alive while this script is
    # started. Waiting here gives it time to save and exit through its normal
    # shutdown path; the script never force-kills an inventory owner.
    Wait-InventatoryProcessesStopped -timeoutSeconds 60
    $tag = $ReleaseVersion
  } else {
    New-Item -ItemType Directory -Path $downloadRoot | Out-Null
    if ($ReleaseTag) {
      $releaseBase = "https://github.com/$repo/releases/download/$ReleaseTag"
    } else {
      $releaseBase = "https://github.com/$repo/releases/latest/download"
    }
    Invoke-WebRequest -UseBasicParsing -Uri "$releaseBase/Inventatory-win-x64.zip" -OutFile (Join-Path $downloadRoot 'Inventatory-win-x64.zip')
    Invoke-WebRequest -UseBasicParsing -Uri "$releaseBase/SHA256SUMS.txt" -OutFile (Join-Path $downloadRoot 'SHA256SUMS.txt')
    $tag = 'latest public beta'
    $archive = Join-Path $downloadRoot 'Inventatory-win-x64.zip'
    $checksums = Join-Path $downloadRoot 'SHA256SUMS.txt'
    Verify-UpdateAsset $archive $checksums 'Inventatory-win-x64.zip'
    Quiesce-InventatoryProcesses
  }

  Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
  Expand-Archive -LiteralPath $archive -DestinationPath $stagingRoot -Force
  $packageRoot = Join-Path $stagingRoot 'Inventatory'
  if (-not (Test-Path (Join-Path $packageRoot 'inventatory.exe')) -or
      -not (Test-Path (Join-Path $packageRoot 'inventatory-background.exe'))) {
    throw 'Release archive is missing Inventatory or its background launcher.'
  }

  # Re-check immediately before replacing files. This closes the race between
  # the initial prompt and the directory swap without terminating a process
  # that may still be saving inventory data.
  Wait-InventatoryProcessesStopped -timeoutSeconds 1

  $oldInstallMoved = $false
  $newInstallMoveAttempted = $false
  try {
    if (Test-Path -LiteralPath $installRoot) {
      Move-Item -LiteralPath $installRoot -Destination $backupRoot -ErrorAction Stop
      $oldInstallMoved = $true
    }
    $newInstallMoveAttempted = $true
    if ($TestMode -and $TestActivationFailure) { throw 'Injected activation failure for installer smoke coverage.' }
    Move-Item -LiteralPath $packageRoot -Destination $installRoot -ErrorAction Stop
    $updateActivationCompleted = $true
    Remove-Item -LiteralPath $backupRoot -Recurse -Force -ErrorAction SilentlyContinue
  } catch {
    # Keep the old installation recoverable if the new directory cannot be
    # activated. The staged package is disposable; user data is elsewhere.
    if ($newInstallMoveAttempted -and (Test-Path -LiteralPath $installRoot)) {
      $failedInstallRoot = "$installRoot.failed." + [guid]::NewGuid().ToString('N')
      try {
        Move-Item -LiteralPath $installRoot -Destination $failedInstallRoot -ErrorAction Stop
      } catch {
        throw "Installation failed and the new files could not be moved aside for rollback: $($_.Exception.Message)"
      }
    }
    if ($oldInstallMoved -and (Test-Path -LiteralPath $backupRoot)) {
      try {
        Move-Item -LiteralPath $backupRoot -Destination $installRoot -ErrorAction Stop
      } catch {
        throw "Installation failed and rollback could not restore the existing installation: $($_.Exception.Message)"
      }
    }
    throw
  }

  $exe = Join-Path $installRoot 'inventatory.exe'
  if ($UpdateMode) {
    try {
      Write-UpdateStatus 'complete' $ReleaseVersion
    } catch {
      Write-Warning "The update completed, but its completion marker could not be written: $($_.Exception.Message)"
    }
    Write-Host "Inventatory $ReleaseVersion installed."
    Start-InventatoryTerminal $exe $installRoot
  } else {
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
    $programs = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
    New-Shortcut (Join-Path $programs 'Inventatory.lnk') $exe '' $installRoot
    $makeDesktop = $DesktopShortcut
    if (-not $DesktopShortcut) { $makeDesktop = (Read-Host 'Create a desktop shortcut? [y/N]') -match '^[Yy]' }
    if ($makeDesktop) { New-Shortcut (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Inventatory.lnk') $exe '' $installRoot }

    Write-Host "Inventatory $tag installed for this Windows user."
    if (-not $NoLaunch) { Start-InventatoryAfterCountdown $exe $installRoot }
  }
} catch {
  if ($UpdateMode) {
    $failure = $_.Exception.Message
    if (-not $updateActivationCompleted) {
      try { Wait-InventatoryProcessesStopped -timeoutSeconds 60 } catch { }
    }
    try { Write-UpdateStatus 'failed' $ReleaseVersion $failure } catch { }
    $relaunch = if ($updateActivationCompleted) { Join-Path $installRoot 'inventatory.exe' } else { $oldInstallExe }
    if (Test-Path -LiteralPath $relaunch) {
      try { Start-InventatoryTerminal $relaunch $installRoot } catch { }
    }
  }
  throw
} finally {
  if ($downloadRoot) { Remove-Item -LiteralPath $downloadRoot -Recurse -Force -ErrorAction SilentlyContinue }
  Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
  if ($UpdateMode -and $updatePackageRoot -and (Test-Path -LiteralPath $updatePackageRoot)) {
      Remove-Item -LiteralPath $updatePackageRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}
