@echo off
rem Adds the Windows 11 SDK to Visual Studio 2026 (needed to build any Windows program).
set "OUT=%~dp0install-sdk.log"
echo [%date% %time%] start > "%OUT%"
echo Adding the Windows 11 SDK to Visual Studio - approve the admin prompt.
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files\Microsoft Visual Studio\18\Community" --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --passive --norestart >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"
dir /b "%ProgramFiles(x86)%\Windows Kits\10\Lib" >> "%OUT%" 2>&1
echo [%date% %time%] done >> "%OUT%"
