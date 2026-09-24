@echo off
setlocal EnableExtensions DisableDelayedExpansion

if not exist "%LOCALAPPDATA%\Programs" mkdir "%LOCALAPPDATA%\Programs"
if errorlevel 1 goto :failed

set "INSTALLER_PATH=%TEMP%\Install-Inventatory-%RANDOM%-%RANDOM%.ps1"
curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Install-Inventatory.ps1" -o "%INSTALLER_PATH%"
if errorlevel 1 goto :failed

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%INSTALLER_PATH%"
set "INVENTATORY_EXIT_CODE=%ERRORLEVEL%"
del /q "%INSTALLER_PATH%" >nul 2>&1
exit /b %INVENTATORY_EXIT_CODE%

:failed
if defined INSTALLER_PATH del /q "%INSTALLER_PATH%" >nul 2>&1
echo.
echo Inventatory could not be downloaded or installed.
echo The error is shown above. Press any key to close this window.
pause >nul
exit /b 1
