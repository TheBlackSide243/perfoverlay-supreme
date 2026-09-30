@echo off
setlocal
title Ricompila PerfOverlay Supreme

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERRORE: CMake non trovato. Installa con:
    echo   winget install Kitware.CMake
    pause
    exit /b 1
)

echo Chiudo PerfOverlay Supreme se aperto...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0src\tools\close-monitor.ps1"
tasklist /fi "imagename eq PerfOverlaySupreme.exe" | find /i "PerfOverlaySupreme.exe" >nul
if not errorlevel 1 (
    echo.
    echo PerfOverlaySupreme.exe e' ancora aperto.
    echo Chiudilo dall'icona vicino all'orologio: tasto destro ^> Esci,
    echo poi premi un tasto per continuare.
    pause >nul
)

echo Configuro...
cmake -S "%~dp0src" -B "%~dp0src\build" -A x64
if errorlevel 1 goto :fail

echo Compilo...
cmake --build "%~dp0src\build" --config Release
if errorlevel 1 goto :fail

copy /y "%~dp0src\build\bin\Release\PerfOverlaySupreme.exe" "%~dp0" >nul
echo.
echo Fatto: PerfOverlaySupreme.exe aggiornato (overlay, impostazioni e test in un solo file).
pause
exit /b 0

:fail
echo.
echo ERRORE durante la compilazione, vedi i messaggi sopra.
pause
exit /b 1
