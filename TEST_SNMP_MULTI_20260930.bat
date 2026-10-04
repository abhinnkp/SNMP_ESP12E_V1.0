@echo off
setlocal EnableExtensions
title ETPL SNMP Multi-Device Tester
set "ROOT=%~dp0"
pushd "%ROOT%"
if errorlevel 1 exit /b 1
echo ============================================================
echo  ETPL SNMP MULTI-DEVICE TESTER - 30 SEPTEMBER 2026
echo  Add devices, explicit IP ranges or CSV inside the app.
echo  SNMP v2c / UDP 161 / built-in engine / no Net-SNMP install.
echo  Keep this process running while testing.
echo ============================================================
where py >nul 2>&1
if not errorlevel 1 (
  py -3 -B "%ROOT%tools\multi_snmp_dashboard.py" %*
  goto FINISH
)
where python >nul 2>&1
if not errorlevel 1 (
  python -B "%ROOT%tools\multi_snmp_dashboard.py" %*
  goto FINISH
)
echo Python 3.10 or newer is required. Install Python and run this batch again.
pause
popd
exit /b 1
:FINISH
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" pause
popd
exit /b %RESULT%
