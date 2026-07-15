$ErrorActionPreference = 'Stop'
$installRoot = Join-Path $env:LOCALAPPDATA 'Programs\Inventatory'
$bootstrapDownloadRoot = Join-Path $env:TEMP 'Inventatory-install'
if (Get-Process inventatory -ErrorAction SilentlyContinue) { throw 'Close Inventatory before uninstalling.' }
Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Inventatory.lnk') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Inventatory.lnk') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $bootstrapDownloadRoot -Recurse -Force -ErrorAction SilentlyContinue
if ((Read-Host 'Also delete Documents\Inventatory and local settings? [y/N]') -match '^[Yy]') {
  Remove-Item -LiteralPath (Join-Path $env:USERPROFILE 'Documents\Inventatory') -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath (Join-Path $env:LOCALAPPDATA 'Inventatory') -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'Inventatory was removed.'
