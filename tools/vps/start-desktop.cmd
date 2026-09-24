@echo off
rem Starts the Muuk app with the first *.local.json config in this folder and connects.
cd /d "%~dp0"
for %%f in (*.local.json) do (
  start "" muuk-desktop.exe --config "%~dp0%%f" --connect
  goto :eof
)
echo No *.local.json config found next to this file.
pause
