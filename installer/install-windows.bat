@echo off
:: Installs every *.vst3 folder next to this script into the system VST3 folder (needs admin).
:: Shipped inside the Windows zip; the plugin's UPDATE banner launches it after downloading.
setlocal
set "SRC=%~dp0"
set "DST=%CommonProgramFiles%\VST3"
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Asking for administrator rights to copy into "%DST%" ...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
echo.
echo  Close FL Studio (and any other DAW) before continuing, otherwise the plugin file is locked.
echo.
pause
if not exist "%DST%" mkdir "%DST%"
for /d %%P in ("%SRC%*.vst3") do (
    echo Installing %%~nxP ...
    if exist "%DST%\%%~nxP" rmdir /s /q "%DST%\%%~nxP"
    xcopy "%%P" "%DST%\%%~nxP\" /E /I /Y /Q >nul
)
if exist "%SRC%onnxruntime.dll" (
    for /d %%P in ("%SRC%*.vst3") do (
        for /d %%A in ("%DST%\%%~nxP\Contents\x86_64-win") do copy /Y "%SRC%onnxruntime.dll" "%%A\" >nul
    )
)
echo.
echo  Done. In FL Studio: Options ^> Manage plugins ^> tick "Rescan previously verified plugins" ^> Start scan.
echo.
pause
