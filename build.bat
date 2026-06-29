@echo off
title StealthOverlay Build
echo ============================================================
echo  StealthOverlay Build Script
echo ============================================================
echo.

set MSBUILD=
for %%E in (Enterprise Professional Community BuildTools) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe" (
        set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\%%E\MSBuild\Current\Bin\MSBuild.exe"
        echo Found VS 2022 %%E
        goto :found
    )
)
for %%E in (Enterprise Professional Community BuildTools) do (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\%%E\MSBuild\Current\Bin\MSBuild.exe" (
        set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\%%E\MSBuild\Current\Bin\MSBuild.exe"
        echo Found VS 2019 %%E
        goto :found
    )
)
echo ERROR: MSBuild not found.
pause
exit /b 1

:found
echo.
echo Killing any running instance...
taskkill /f /im StealthOverlayApp.exe >nul 2>&1
timeout /t 1 /nobreak >nul
echo.
echo Building Debug x64...
echo ============================================================

"%MSBUILD%" "StealthOverlay\StealthOverlay.sln" /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m /verbosity:normal /nologo > build_output.log 2>&1

if %ERRORLEVEL% == 0 (
    echo BUILD SUCCEEDED
    echo.
    echo Launching app...
    start "" "StealthOverlay\x64\Debug\StealthOverlayApp.exe"
) else (
    echo BUILD FAILED -- opening log
    type build_output.log
    echo.
)
pause
