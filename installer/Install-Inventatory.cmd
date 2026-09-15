@echo off
setlocal EnableExtensions DisableDelayedExpansion

if not exist "%LOCALAPPDATA%\Programs" mkdir "%LOCALAPPDATA%\Programs"
if errorlevel 1 goto :failed

curl.exe -fL "https://github.com/Kwiatens/Inventatory-Software/releases/latest/download/Inventatory-win-x64.zip" -o "%TEMP%\Inventatory-win-x64.zip"
if errorlevel 1 goto :failed

tar.exe -xf "%TEMP%\Inventatory-win-x64.zip" -C "%LOCALAPPDATA%\Programs"
if errorlevel 1 goto :failed

del /q "%TEMP%\Inventatory-win-x64.zip"
if errorlevel 1 goto :failed

"%LOCALAPPDATA%\Programs\Inventatory\inventatory.exe"
set "INVENTATORY_EXIT_CODE=%ERRORLEVEL%"
exit /b %INVENTATORY_EXIT_CODE%

:failed
echo.
echo Inventatory could not be downloaded or started.
echo The error is shown above. Press any key to close this window.
pause >nul
exit /b 1
