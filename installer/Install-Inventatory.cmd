@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Inventatory.ps1" %*
exit /b %errorlevel%
