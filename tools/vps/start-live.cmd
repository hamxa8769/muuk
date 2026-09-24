@echo off
rem Runs the console bot with the first *.local.json config in this folder.
rem Ctrl+C or closing this window closes every position first (see README-VPS.md).
cd /d "%~dp0"
for %%f in (*.local.json) do (
  muuk-live.exe --credentials "%~dp0%%f"
  goto :eof
)
echo No *.local.json config found next to this file.
pause
