@echo off
set "OUT=%~dp0check-sdk.log"
echo == Windows Kits folder > "%OUT%"
dir /b "%ProgramFiles(x86)%\Windows Kits\10\Lib" >> "%OUT%" 2>&1
echo == SDK registry >> "%OUT%"
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0" /v InstallationFolder >> "%OUT%" 2>&1
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0" /v ProductVersion >> "%OUT%" 2>&1
echo == Visual Studio installs >> "%OUT%"
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -all -prerelease -products * -format text -property displayName >> "%OUT%" 2>&1
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -all -prerelease -products * -format text -property installationPath >> "%OUT%" 2>&1
echo == C++ desktop workload >> "%OUT%"
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -all -prerelease -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath >> "%OUT%" 2>&1
echo == done >> "%OUT%"
