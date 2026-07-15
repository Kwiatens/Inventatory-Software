@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-Inventatory.ps1"
exit /b %errorlevel%
