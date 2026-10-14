@echo off
rem Arch Linux for Xiaomi Pad 8 - installer for Windows.
rem Put this file in the same folder as the downloaded .img files and double-click it.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title Arch Linux for Xiaomi Pad 8 - installer
echo.
echo  Arch Linux for Xiaomi Pad 8 - installer
echo  ========================================
echo.

rem 1. fastboot: Google's official tool, downloaded once into this folder.
set FB=platform-tools\fastboot.exe
if not exist "%FB%" (
    echo Downloading Google's fastboot tool...
    powershell -NoProfile -Command "Invoke-WebRequest -Uri 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile 'platform-tools.zip'; Expand-Archive -Force 'platform-tools.zip' '.'; Remove-Item 'platform-tools.zip'"
    if not exist "%FB%" goto :nodl
)

rem 2. The files.
if not exist boot.img goto :missing
if not exist init_boot.img goto :missing
if not exist userdata.part1.img goto :missing

rem 3. The tablet.
echo Now put the tablet in FASTBOOT mode:
echo   1. Turn the tablet OFF.
echo   2. Hold POWER + VOLUME DOWN together until you see the bunny (FASTBOOT).
echo   3. Connect it to this computer with a USB-C cable.
echo.
echo Waiting for the tablet... (if nothing happens, install the Xiaomi USB driver,
echo for example by installing Mi Unlock or Mi Flash, then run this again)
:wait
"%FB%" devices 2>nul | findstr /c:"fastboot" >nul || (timeout /t 1 >nul & goto :wait)

for /f "tokens=2" %%a in ('"%FB%" getvar product 2^>^&1 ^| findstr /b "product:"') do set PRODUCT=%%a
for /f "tokens=2" %%a in ('"%FB%" getvar unlocked 2^>^&1 ^| findstr /b "unlocked:"') do set UNLOCKED=%%a
if not "!PRODUCT!"=="yupei" (
    echo.
    echo Problem: this is not a Xiaomi Pad 8 ^(it says "!PRODUCT!"^). Nothing was changed.
    goto :end
)
if not "!UNLOCKED!"=="yes" (
    echo.
    echo Problem: the bootloader is locked. Unlock it first with Mi Unlock. Nothing was changed.
    goto :end
)

echo.
echo Found your Xiaomi Pad 8.
echo This will ERASE EVERYTHING on the tablet (Android, apps, photos) and install Arch Linux.
set /p OK=Type YES and press Enter to continue: 
if not "!OK!"=="YES" (
    echo You did not type YES. Nothing was changed.
    goto :end
)

echo.
echo Installing... (do not unplug the tablet, this takes a few minutes)
"%FB%" flash boot boot.img || goto :flashfail
"%FB%" flash init_boot init_boot.img || goto :flashfail
for /L %%n in (1,1,20) do (
    if exist userdata.part%%n.img (
        echo Installing the system, part %%n...
        "%FB%" flash userdata userdata.part%%n.img || goto :flashfail
    )
)
"%FB%" reboot
echo.
echo Done! The tablet is starting Arch Linux.
echo The first start takes about a minute. Then it asks for your language,
echo Wi-Fi and a name and password for you. Have fun!
goto :end

:nodl
echo Problem: could not download fastboot. Is the internet connected?
goto :end
:missing
echo Problem: some files are missing. Put ALL the downloaded files in this folder:
echo   boot.img, init_boot.img, userdata.part1.img, userdata.part2.img, ...
goto :end
:flashfail
echo.
echo Problem: flashing failed. Keep the tablet in fastboot, unplug and replug the
echo cable, and run this again.
:end
echo.
pause
