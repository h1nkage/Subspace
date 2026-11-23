@echo off
rem Builds Subspace with rollback NetPlay (Subspace.exe + SubspaceNoGUI.exe).
rem Everything is logged to build.log next to this file.
setlocal
cd /d "%~dp0"
set "LOG=%~dp0build.log"
echo [%date% %time%] start > "%LOG%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo STEP vs: vswhere.exe not found - is Visual Studio 2026 installed? >> "%LOG%"
  goto :fail
)
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo STEP vs: no Visual Studio with the C++ tools found >> "%LOG%"
  goto :fail
)
echo Visual Studio: %VSPATH% >> "%LOG%"
rem No telemetry helper: it lingers and would keep the log file locked.
set VSCMD_SKIP_SENDTELEMETRY=1
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "%~dp0build-vsdevcmd.log" 2>&1

rem Git is optional: Visual Studio's own copy if installed, else downloads use curl.
set "HAVEGIT="
set "VSGIT=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd"
where git >nul 2>&1 && set "HAVEGIT=1"
if not defined HAVEGIT if exist "%VSGIT%\git.exe" set "PATH=%VSGIT%;%PATH%"
if not defined HAVEGIT if exist "%VSGIT%\git.exe" set "HAVEGIT=1"
if defined HAVEGIT (echo git: found >> "%LOG%") else (echo git: not installed, using curl >> "%LOG%")
rem CMake: on PATH, else Visual Studio's bundled copy, else a standalone install.
set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where cmake >nul 2>&1 || if exist "%VSCMAKE%\cmake.exe" set "PATH=%VSCMAKE%;%PATH%"
where cmake >nul 2>&1 || if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
rem Else a portable CMake 4.3.1 in build-tools\ (official zip, checked against its published SHA-256).
set "PCMAKE=%~dp0build-tools\cmake-4.3.1-windows-x86_64\bin"
where cmake >nul 2>&1 || if exist "%PCMAKE%\cmake.exe" set "PATH=%PCMAKE%;%PATH%"
where cmake >nul 2>&1 && goto :havecmake
echo Getting a portable CMake...
echo cmake: downloading portable CMake 4.3.1 >> "%LOG%"
if not exist "%~dp0build-tools" mkdir "%~dp0build-tools"
curl -L --fail -o "%~dp0build-tools\cmake.zip" https://github.com/Kitware/CMake/releases/download/v4.3.1/cmake-4.3.1-windows-x86_64.zip >> "%LOG%" 2>&1 || (echo STEP tools: CMake download failed >> "%LOG%" & goto :fail)
certutil -hashfile "%~dp0build-tools\cmake.zip" SHA256 | findstr /i "03a610be931546474e46a442d171ef10532f4fc847de9cf76b7a8065bbef6a23" >nul || (echo STEP tools: CMake download has the wrong checksum >> "%LOG%" & goto :fail)
tar -xf "%~dp0build-tools\cmake.zip" -C "%~dp0build-tools" >> "%LOG%" 2>&1 || (echo STEP tools: CMake unpack failed >> "%LOG%" & goto :fail)
del "%~dp0build-tools\cmake.zip"
set "PATH=%PCMAKE%;%PATH%"
:havecmake
where cmake >> "%LOG%" 2>&1 || (echo STEP tools: cmake not found >> "%LOG%" & goto :fail)
cmake --version >> "%LOG%" 2>&1

echo === STEP 1: Qt package >> "%LOG%"
echo Step 1/3: downloading Dolphin's Qt package (large)...
if exist "Externals\Qt\Qt6*" goto :qtdone
if defined HAVEGIT goto :qtgit
rem The exact Qt package commit this source pins (the Externals/Qt submodule).
curl -L --fail -o "%TEMP%\ext-win-qt.tar.gz" https://codeload.github.com/dolphin-emu/ext-win-qt/tar.gz/306289108d29a4b31a5460f7b55d28fde7b96d1b >> "%LOG%" 2>&1 || (echo STEP 1 FAILED: download >> "%LOG%" & goto :fail)
if not exist "Externals\Qt" mkdir "Externals\Qt"
tar -xzf "%TEMP%\ext-win-qt.tar.gz" -C "Externals\Qt" --strip-components=1 >> "%LOG%" 2>&1 || (echo STEP 1 FAILED: unpack >> "%LOG%" & goto :fail)
del "%TEMP%\ext-win-qt.tar.gz"
goto :qtdone
:qtgit
git -c core.symlinks=false submodule update --init --depth 1 Externals/Qt >> "%LOG%" 2>&1 || (echo STEP 1 FAILED >> "%LOG%" & goto :fail)
:qtdone
dir /b "Externals\Qt" >> "%LOG%" 2>&1

rem The name the NetPlay window shows as the Revision (and the title bar). Empty: your git tag
rem (setup-my-git.bat), e.g. subspace-1.0. Set it to use a name of your own instead.
set "SUBSPACE_VERSION_NAME="

echo === STEP 2: configure >> "%LOG%"
echo Step 2/3: configuring...
rem Without git Dolphin can't read its own version; name it here (base commit + this branch).
set "SCM="
if not defined HAVEGIT set "SCM=-DDOLPHIN_WC_REVISION=73fdf1a870c8a0ccb5f6a65cce48d99419a0d030 -DDOLPHIN_WC_DESCRIBE=subspace-0.3.103-netplay -DDOLPHIN_WC_BRANCH=dolphin-netplay"
cmake --preset visualstudio-release-x64 -DENABLE_QT=ON -DENABLE_NOGUI=ON -DUSE_SYSTEM_LIBS=OFF -DENCODE_FRAMEDUMPS=OFF -DENABLE_LLVM=OFF -DENABLE_ANALYTICS=OFF -DENABLE_AUTOUPDATE=OFF -DUSE_DISCORD_PRESENCE=OFF -DUSE_RETRO_ACHIEVEMENTS=OFF -DUSE_MGBA=OFF -DUSE_UPNP=OFF -DENABLE_VULKAN=OFF -DENABLE_CLI_TOOL=OFF -DSUBSPACE_NEUTRAL_PATHS=ON %SCM% >> "%LOG%" 2>&1 || (echo STEP 2 FAILED >> "%LOG%" & goto :fail)

echo === STEP 3: build >> "%LOG%"
echo Step 3/3: building Subspace.exe and SubspaceNoGUI.exe (this takes a while)...
cmake --build --preset visualstudio-build-release-x64 --target dolphin-emu dolphin-nogui --parallel >> "%LOG%" 2>&1 || (echo STEP 3 FAILED >> "%LOG%" & goto :fail)

echo [%date% %time%] BUILD OK >> "%LOG%"
echo.
echo Build finished: build\release\x64\Binaries\Subspace.exe
pause
exit /b 0

:fail
echo [%date% %time%] BUILD FAILED >> "%LOG%"
echo.
echo Build failed - see build.log
pause
exit /b 1
