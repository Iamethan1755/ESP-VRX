@echo off
set PORT=COM10
if not "%~1"=="" set PORT=%~1
echo Opening C5VRX serial monitor on %PORT%...
echo Press Ctrl+] or Ctrl+C to exit.
echo Commands you can send:
echo   g auto   - set RF gain to automatic
echo   g <num>  - set RF gain manually (e.g. g 52 or g 62)
echo   s        - print receiver telemetry snapshot
echo   p        - print hardware probe / loopback status
echo.
python tools\monitor.py %PORT%
