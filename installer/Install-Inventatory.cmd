@echo off
setlocal
where gh >nul 2>nul || (
  echo GitHub CLI is required for the private Inventatory beta.
  echo Install it from https://cli.github.com/, sign in with ^"gh auth login^", then run this installer again.
  exit /b 1
)
gh auth status -h github.com >nul 2>nul || (
  echo Sign in to the GitHub account invited to Kwiatens/Inventatory-Software with ^"gh auth login^" first.
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Inventatory.ps1" %*
exit /b %errorlevel%
