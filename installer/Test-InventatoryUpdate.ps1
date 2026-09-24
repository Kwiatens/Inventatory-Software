[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$installerSource = (Resolve-Path (Join-Path $PSScriptRoot 'Install-Inventatory.ps1')).Path
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('Inventatory-update-smoke-' + [guid]::NewGuid().ToString('N'))
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Assert-DirectInstallCommand {
  $releaseTag = 'v0.2.0-rc.3'
  $releaseTagPattern = [regex]::Escape($releaseTag)
  $readmeCommand = (Get-Content (Join-Path $repositoryRoot 'README.md') |
    Where-Object { $_ -match 'curl\.exe ' } | Select-Object -First 1)
  if (-not $readmeCommand -or $readmeCommand -notmatch
      "releases/download/$releaseTagPattern/Install-Inventatory\.ps1" -or
      $readmeCommand -notmatch 'powershell\.exe .*ExecutionPolicy Bypass .*Install-Inventatory\.ps1' -or
      $readmeCommand -match 'Inventatory-win-x64\.zip|tar\.exe') {
    throw 'The Windows prerelease command must run the checksum-verifying installer from its exact release tag.'
  }

  $readmeLinuxCommand = (Get-Content (Join-Path $repositoryRoot 'README.md') |
    Where-Object { $_ -match 'Inventatory-linux-x64\.tar\.gz' } | Select-Object -First 1)
  if (-not $readmeLinuxCommand -or $readmeLinuxCommand -match 'releases/latest/download' -or
      $readmeLinuxCommand -notmatch "releases/download/$releaseTagPattern/Inventatory-linux-x64\.tar\.gz" -or
      $readmeLinuxCommand -notmatch "releases/download/$releaseTagPattern/SHA256SUMS-linux\.txt" -or
      $readmeLinuxCommand -notmatch "releases/download/$releaseTagPattern/Install-Inventatory\.sh") {
    throw 'The Linux prerelease command must download all assets from its exact release tag.'
  }

  $publicBetaCommand = (Get-Content (Join-Path $repositoryRoot 'docs/public-beta.md') |
    Where-Object { $_ -match 'curl\.exe ' } | Select-Object -First 1)
  if (-not $publicBetaCommand -or
      $publicBetaCommand -notmatch "releases/download/$releaseTagPattern/Install-Inventatory\.ps1" -or
      $publicBetaCommand -notmatch 'powershell\.exe .*ExecutionPolicy Bypass .*Install-Inventatory\.ps1') {
    throw 'The public beta Windows command must install from the exact prerelease tag.'
  }

  $publicBetaLinuxCommand = (Get-Content (Join-Path $repositoryRoot 'docs/public-beta.md') |
    Where-Object { $_ -match 'Inventatory-linux-x64\.tar\.gz' } | Select-Object -First 1)
  if (-not $publicBetaLinuxCommand -or $publicBetaLinuxCommand -match 'releases/latest/download' -or
      $publicBetaLinuxCommand -notmatch "releases/download/$releaseTagPattern/Inventatory-linux-x64\.tar\.gz" -or
      $publicBetaLinuxCommand -notmatch "releases/download/$releaseTagPattern/SHA256SUMS-linux\.txt" -or
      $publicBetaLinuxCommand -notmatch "releases/download/$releaseTagPattern/Install-Inventatory\.sh") {
    throw 'The public beta Linux command must download all assets from the exact prerelease tag.'
  }

  $launcher = Get-Content -Raw (Join-Path $PSScriptRoot 'Install-Inventatory.cmd')
  if ($launcher -notmatch 'curl\.exe' -or
      $launcher -notmatch 'powershell\.exe .*ExecutionPolicy Bypass .*INSTALLER_PATH' -or
      $launcher -notmatch 'Install-Inventatory\.ps1' -or
      $launcher -match 'tar\.exe|Inventatory-win-x64\.zip' -or
      $launcher -match '__INVENTATORY_RELEASE_') {
    throw 'The CMD bootstrap must run the checksum-verifying PowerShell installer.'
  }

  $installer = Get-Content -Raw (Join-Path $PSScriptRoot 'Install-Inventatory.ps1')
  if (-not $installer.Contains("[string]`$ReleaseTag = ''") -or
      $installer -notmatch 'releases/download/\$ReleaseTag') {
    throw 'The Windows installer must support a release-tag-pinned package download.'
  }

  $releaseWorkflow = Get-Content -Raw (Join-Path $repositoryRoot '.github/workflows/release.yml')
  if (-not $releaseWorkflow.Contains('RELEASE_TAG: ${{ needs.validate.outputs.tag }}') -or
      -not $releaseWorkflow.Contains('$publishedReleaseTagDeclaration') -or
      -not $releaseWorkflow.Contains('$taggedInstallerUrl')) {
    throw 'The release package must bind both Windows installer assets to the validated release tag.'
  }
}

function New-UpdateFixture([string]$name, [bool]$validPackage = $true) {
  $fixture = Join-Path $root $name
  $install = Join-Path $fixture 'install'
  $state = Join-Path $fixture 'state'
  $packageWorkspace = Join-Path $fixture ('Inventatory-update-' + $name)
  $packageRoot = Join-Path $packageWorkspace 'Inventatory'
  New-Item -ItemType Directory -Force -Path $install, $state, $packageRoot | Out-Null
  Set-Content -LiteralPath (Join-Path $install 'inventatory.exe') -Value 'old foreground' -NoNewline
  Set-Content -LiteralPath (Join-Path $install 'inventatory-background.exe') -Value 'old background' -NoNewline
  Set-Content -LiteralPath (Join-Path $state 'inventory.db') -Value 'inventory survives' -NoNewline
  Set-Content -LiteralPath (Join-Path $state 'settings.conf') -Value 'settings survive' -NoNewline
  Set-Content -LiteralPath (Join-Path $packageRoot 'inventatory.exe') -Value 'new foreground' -NoNewline
  if ($validPackage) {
    Set-Content -LiteralPath (Join-Path $packageRoot 'inventatory-background.exe') -Value 'new background' -NoNewline
  }
  $archive = Join-Path $packageWorkspace 'Inventatory-win-x64.zip'
  Compress-Archive -Path $packageRoot -DestinationPath $archive -Force
  $scriptCopy = Join-Path $packageWorkspace 'Install-Inventatory.ps1'
  Copy-Item -LiteralPath $installerSource -Destination $scriptCopy -Force
  $archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
  $scriptHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $scriptCopy).Hash.ToLowerInvariant()
  Set-Content -LiteralPath (Join-Path $packageWorkspace 'SHA256SUMS.txt') -Value @(
    "$archiveHash  Inventatory-win-x64.zip"
    "$scriptHash  Install-Inventatory.ps1"
  )
  return [PSCustomObject]@{
    Fixture = $fixture
    Install = $install
    State = $state
    Package = $packageWorkspace
    Archive = $archive
    Checksums = Join-Path $packageWorkspace 'SHA256SUMS.txt'
    Script = $scriptCopy
    Marker = Join-Path $state 'update-status.conf'
    Notes = Join-Path $state 'update-notes.md'
  }
}

function Invoke-UpdateFixture($fixture, [switch]$ActivationFailure) {
  $launchMarker = Join-Path $fixture.State 'relaunch.txt'
  Set-Content -LiteralPath $fixture.Marker -Value "schema_version=1`nstate=pending`nversion=v1.2.3`nerror=`n" -NoNewline
  $arguments = @{
    Repository = 'Kwiatens/Inventatory-Software'
    UpdateMode = $true
    TestMode = $true
    TestInstallRoot = $fixture.Install
    ArchivePath = $fixture.Archive
    ChecksumsPath = $fixture.Checksums
    ReleaseVersion = 'v1.2.3'
    CompletionPath = $fixture.Marker
    NotesPath = $fixture.Notes
    TestLaunchMarker = $launchMarker
  }
  if ($ActivationFailure) { $arguments.TestActivationFailure = $true }
  $failed = $false
  $errorText = ''
  try {
    & $fixture.Script @arguments
  } catch {
    $failed = $true
    $errorText = $_.Exception.Message
  }
  return [PSCustomObject]@{ Failed = $failed; Error = $errorText; LaunchMarker = $launchMarker }
}

try {
  Assert-DirectInstallCommand
  New-Item -ItemType Directory -Force -Path $root | Out-Null

  $successFixture = New-UpdateFixture 'success'
  Set-Content -LiteralPath $successFixture.Notes -Value "Release notes survive restart." -NoNewline
  $success = Invoke-UpdateFixture $successFixture
  if ($success.Failed) { throw "Successful update smoke test failed: $($success.Error)" }
  if ((Get-Content -Raw $successFixture.Marker) -notmatch 'state=complete' -or
      (Get-Content -Raw (Join-Path $successFixture.Install 'inventatory.exe')) -ne 'new foreground' -or
      (Get-Content -Raw (Join-Path $successFixture.State 'inventory.db')) -ne 'inventory survives' -or
      (Get-Content -Raw (Join-Path $successFixture.State 'settings.conf')) -ne 'settings survive' -or
      (Get-Content -Raw $successFixture.Notes) -ne 'Release notes survive restart.' -or
      (Get-Content -Raw $success.LaunchMarker) -ne (Join-Path $successFixture.Install 'inventatory.exe') -or
      (Test-Path -LiteralPath $successFixture.Package) -or (Test-Path -LiteralPath "$($successFixture.Install).staging")) {
    throw 'Successful update smoke test failed.'
  }

  $runningFixture = New-UpdateFixture 'running'
  $cmdCopy = Join-Path $runningFixture.Install 'inventatory.exe'
  Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\cmd.exe') -Destination $cmdCopy -Force
  # Leave time for a cold WMI query and archive verification before the updater checks the process.
  $heldProcess = Start-Process -FilePath $cmdCopy -ArgumentList @('/c', 'ping -n 15 127.0.0.1 > nul') -WindowStyle Hidden -PassThru
  $runningResult = Invoke-UpdateFixture $runningFixture
  if ($runningResult.Failed -or (Get-Content -Raw $runningResult.LaunchMarker) -notmatch 'inventatory.exe') {
    throw 'Running-process wait smoke test failed.'
  }
  if (-not $heldProcess.HasExited) { Stop-Process -Id $heldProcess.Id -Force }

  $stagingFixture = New-UpdateFixture 'staging-failure' $false
  $stagingResult = Invoke-UpdateFixture $stagingFixture
  if (-not $stagingResult.Failed -or (Get-Content -Raw $stagingFixture.Marker) -notmatch 'state=failed' -or
      (Get-Content -Raw (Join-Path $stagingFixture.Install 'inventatory.exe')) -ne 'old foreground' -or
      (Get-Content -Raw (Join-Path $stagingFixture.State 'settings.conf')) -ne 'settings survive') {
    throw 'Staging failure smoke test failed.'
  }

  $rollbackFixture = New-UpdateFixture 'rollback'
  $rollbackResult = Invoke-UpdateFixture $rollbackFixture -ActivationFailure
  if (-not $rollbackResult.Failed -or (Get-Content -Raw $rollbackFixture.Marker) -notmatch 'state=failed' -or
      (Get-Content -Raw (Join-Path $rollbackFixture.Install 'inventatory.exe')) -ne 'old foreground' -or
      (Get-Content -Raw $rollbackResult.LaunchMarker) -notmatch 'inventatory.exe') {
    throw 'Rollback smoke test failed.'
  }

  'Inventatory installer update smoke tests passed.'
} finally {
  Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
