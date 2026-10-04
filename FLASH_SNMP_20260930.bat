@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

set "TITLE_TEXT=ETPL SNMP Field Engineer Fast BIN Flash Tool"
set "TOOL_VERSION=SNMP_NETWORK_SERVICE_20260930"
set "EXPECTED_SHA256=2BACDE07C55874441B62C2F60039A07DF1DBE0C7D556F84FC59A4296ADF88780"
title %TITLE_TEXT%

set "ROOT=%~dp0"
set "PUSHD_OK=0"
pushd "%ROOT%" >nul 2>&1
if errorlevel 1 goto ERR_FOLDER
set "PUSHD_OK=1"

set "BIN=%ROOT%ETPL_SNMP_NETWORK_20260930.bin"
set "LOG=%ROOT%fast_bin_flash_log.txt"
set "OUT=%ROOT%fast_bin_flash_output.txt"
set "HASH_OUT=%ROOT%fast_bin_hash.txt"

> "%LOG%" echo %TITLE_TEXT%
>> "%LOG%" echo Version: %TOOL_VERSION%
>> "%LOG%" echo Started: %DATE% %TIME%
>> "%LOG%" echo Folder: %ROOT%
>> "%LOG%" echo Firmware: %BIN%

echo ============================================================
echo  %TITLE_TEXT%
echo ============================================================
echo  VERSION: %TOOL_VERSION%
echo  PREBUILT BIN: no PlatformIO build or compile
echo  USE ONLY: FLASH_SNMP_20260930.bat
echo ============================================================
echo.

if not exist "%BIN%" goto ERR_BIN

set "PYTHON="
set "PY312=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
set "PY311=%LOCALAPPDATA%\Programs\Python\Python311\python.exe"

if exist "%PY312%" (
  "%PY312%" -c "import sys" >nul 2>&1
  if not errorlevel 1 set "PYTHON=%PY312%"
)
if defined PYTHON goto PY_FOUND

if exist "%PY311%" (
  "%PY311%" -c "import sys" >nul 2>&1
  if not errorlevel 1 set "PYTHON=%PY311%"
)
if defined PYTHON goto PY_FOUND

where py >nul 2>&1
if not errorlevel 1 (
  for /f "delims=" %%P in ('py -3 -c "import sys; print(sys.executable)" 2^>nul') do set "PYTHON=%%P"
)
if defined PYTHON goto PY_FOUND

where python >nul 2>&1
if not errorlevel 1 (
  for /f "delims=" %%P in ('python -c "import sys; print(sys.executable)" 2^>nul') do set "PYTHON=%%P"
)
if defined PYTHON goto PY_FOUND

echo Python was not found. Trying automatic installation through winget...
where winget >nul 2>&1
if errorlevel 1 goto ERR_PYTHON
winget install -e --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
if exist "%PY312%" set "PYTHON=%PY312%"
if not defined PYTHON goto ERR_PYTHON

:PY_FOUND
echo Python:
echo   %PYTHON%
>> "%LOG%" echo Python: %PYTHON%

"%PYTHON%" -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest().upper())" "%BIN%" > "%HASH_OUT%" 2>nul
if errorlevel 1 goto ERR_BIN_HASH
set "ACTUAL_SHA256="
set /p "ACTUAL_SHA256=" < "%HASH_OUT%"
if /i not "!ACTUAL_SHA256!"=="%EXPECTED_SHA256%" goto ERR_BIN_HASH
echo Firmware SHA256: VERIFIED
>> "%LOG%" echo Firmware SHA256: !ACTUAL_SHA256! VERIFIED

"%PYTHON%" -c "import importlib.metadata as m,sys; v=m.version('esptool'); print('esptool',v); sys.exit(0 if v.split('.')[0]=='5' else 1)" > "%OUT%" 2>&1
if not errorlevel 1 goto ESPTOOL_READY

echo esptool not found. Installing esptool 5.3.1 automatically...
echo Internet is required only for this first installation.
"%PYTHON%" -m ensurepip --upgrade >> "%LOG%" 2>&1
"%PYTHON%" -m pip install --disable-pip-version-check esptool==5.3.1 >> "%LOG%" 2>&1
if errorlevel 1 goto ERR_ESPTOOL
"%PYTHON%" -m esptool version > "%OUT%" 2>&1
if errorlevel 1 goto ERR_ESPTOOL

:ESPTOOL_READY
type "%OUT%"
type "%OUT%" >> "%LOG%"
if /i "%~1"=="--self-test" goto SELF_TEST_PASS

:ASK_PORT
echo.
echo Available COM ports:
"%PYTHON%" -m serial.tools.list_ports
if errorlevel 1 echo COM list unavailable. Check Device Manager, then enter the port manually.
echo.
set "PORT="
set /p "PORT=Enter ESP COM port, for example COM5: "
if not defined PORT goto ERR_NO_PORT
set "PORT=!PORT: =!"
if "!PORT!"=="" goto ERR_NO_PORT
"%PYTHON%" -c "import sys,serial.tools.list_ports as ports; sys.exit(0 if sys.argv[1].upper() in {p.device.upper() for p in ports.comports()} else 1)" "!PORT!"
if errorlevel 1 (
  echo.
  echo !PORT! is not an available COM port. Select a port from the list above.
  goto ASK_PORT
)
>> "%LOG%" echo COM port: !PORT!

:FLASH_AGAIN
echo.
echo ===================== BOOTLOADER STEPS =====================
echo 1. Disconnect RS485/inverter UART wires during flashing.
echo 2. Connect USB serial/programmer and stable 3.3V power.
echo 3. Connect GPIO0 / BOOT to GND.
echo 4. Press RESET once, or power OFF and ON once.
echo 5. Keep GPIO0 grounded and press ENTER here.
echo.
echo This tool directly flashes the verified BIN at address 0x00000.
echo It does NOT compile and does NOT erase the EEPROM/settings area.
echo ============================================================
set "READY="
set /p "READY=Press ENTER when ESP8266 is in bootloader mode: "

set "ATTEMPT=1"
set "BAUD=115200"
set "BEFORE=default-reset"
set "METHOD=automatic reset, 115200 baud"
call :RUN_FLASH
if "!FLASH_RESULT!"=="0" goto PASS_RESULT
if "!FLASH_RESULT!"=="3" goto ERR_PORT_OPEN

echo.
echo Attempt 1 failed. Keep GPIO0 grounded and press RESET once.
set /p "READY=Press ENTER for manual-reset attempt at 115200: "
set "ATTEMPT=2"
set "BAUD=115200"
set "BEFORE=no-reset"
set "METHOD=manual reset, 115200 baud"
call :RUN_FLASH
if "!FLASH_RESULT!"=="0" goto PASS_RESULT
if "!FLASH_RESULT!"=="3" goto ERR_PORT_OPEN

echo.
echo Attempt 2 failed. Keep GPIO0 grounded and press RESET once.
set /p "READY=Press ENTER for manual slow fallback at 57600: "
set "ATTEMPT=3"
set "BAUD=57600"
set "BEFORE=no-reset"
set "METHOD=manual reset, 57600 baud"
call :RUN_FLASH
if "!FLASH_RESULT!"=="0" goto PASS_RESULT
if "!FLASH_RESULT!"=="3" goto ERR_PORT_OPEN

echo.
echo Attempt 3 failed. Last fallback uses automatic reset at 57600.
set /p "READY=Keep GPIO0 grounded, press RESET, then ENTER: "
set "ATTEMPT=4"
set "BAUD=57600"
set "BEFORE=default-reset"
set "METHOD=automatic reset, 57600 baud"
call :RUN_FLASH
if "!FLASH_RESULT!"=="0" goto PASS_RESULT
if "!FLASH_RESULT!"=="3" goto ERR_PORT_OPEN
goto FAIL_RESULT

:RUN_FLASH
echo.
echo Attempt !ATTEMPT!/4: !METHOD!
echo Flashing prebuilt firmware. Please do not disconnect power.
>> "%LOG%" echo Attempt !ATTEMPT!/4: !METHOD!
"%PYTHON%" -m esptool --chip esp8266 --port "!PORT!" --baud !BAUD! --before !BEFORE! --after no-reset write-flash --flash-mode dout --flash-freq 40m --flash-size 4MB 0x00000 "%BIN%" > "%OUT%" 2>&1
set "FLASH_RESULT=!ERRORLEVEL!"
type "%OUT%"
type "%OUT%" >> "%LOG%"
if not "!FLASH_RESULT!"=="0" (
  findstr /i /c:"Could not open" "%OUT%" >nul 2>&1
  if not errorlevel 1 set "FLASH_RESULT=3"
)
>> "%LOG%" echo Result: !FLASH_RESULT!
exit /b 0

:ERR_PORT_OPEN
echo.
echo !PORT! could not be opened. Check the USB connection and close any
echo serial monitor using this port. Then select the available COM port.
goto ASK_PORT

:PASS_RESULT
echo.
echo ============================================================
echo  PASS - Verified prebuilt BIN flashed successfully.
echo ============================================================
echo.
echo Next:
echo 1. Remove GPIO0 / BOOT from GND.
echo 2. Press RESET or power cycle.
echo 3. Existing saved Ethernet settings are retained.
echo 4. For a fresh controller, connect InverterSetup_XXXXXXXXXXXX.
echo 5. Password: Tangent123, setup page: http://192.168.10.1
echo 6. Save a UNIQUE Ethernet IP or select DHCP.
echo 7. Run TEST_SNMP_20260929.bat over Ethernet.
echo.
echo Log saved: %LOG%
>> "%LOG%" echo PASS - Prebuilt BIN flashed successfully.
set "FINAL_RESULT=0"
set "RETRY_TARGET=FLASH_AGAIN"
goto ASK_RETRY

:FAIL_RESULT
echo.
echo ============================================================
echo  FAIL - ESP8266 did not accept the prebuilt BIN.
echo ============================================================
findstr /i /c:"Failed to connect" /c:"No serial data received" "%OUT%" >nul 2>&1
if not errorlevel 1 (
  echo ESP8266 did not enter bootloader mode or the selected COM port is wrong.
  echo Keep GPIO0 grounded BEFORE reset and leave it grounded during connect.
) else (
  echo Check the detailed esptool message above and in the log.
)
echo Log saved: %LOG%
>> "%LOG%" echo FAIL - Direct BIN flashing failed.
set "FINAL_RESULT=1"
set "RETRY_TARGET=FLASH_AGAIN"
goto ASK_RETRY

:ASK_RETRY
echo.
set "RUN_AGAIN="
set /p "RUN_AGAIN=Retry same COM [Y], change COM [C], or exit [N]: "
if /i "!RUN_AGAIN!"=="Y" goto !RETRY_TARGET!
if /i "!RUN_AGAIN!"=="C" goto ASK_PORT
if /i "!RUN_AGAIN!"=="N" goto EXIT_TOOL
echo Please type Y, C or N.
goto ASK_RETRY

:ERR_FOLDER
echo FAIL - Could not open the batch folder.
set "FINAL_RESULT=1"
set "RETRY_TARGET=EXIT_TOOL"
goto ASK_RETRY

:ERR_BIN
echo FAIL - Prebuilt firmware BIN not found:
echo   %BIN%
set "FINAL_RESULT=1"
set "RETRY_TARGET=EXIT_TOOL"
goto ASK_RETRY

:ERR_BIN_HASH
echo FAIL - Firmware BIN hash is wrong or the file is damaged.
echo Expected: %EXPECTED_SHA256%
echo Actual:   !ACTUAL_SHA256!
set "FINAL_RESULT=1"
set "RETRY_TARGET=EXIT_TOOL"
goto ASK_RETRY

:ERR_PYTHON
echo FAIL - Python was not found and automatic installation was unavailable.
echo Install Python 3.12, then run this batch again.
set "FINAL_RESULT=1"
set "RETRY_TARGET=EXIT_TOOL"
goto ASK_RETRY

:ERR_ESPTOOL
echo FAIL - esptool installation failed.
echo Check internet access and log: %LOG%
set "FINAL_RESULT=1"
set "RETRY_TARGET=PY_FOUND"
goto ASK_RETRY

:ERR_NO_PORT
echo FAIL - No COM port entered.
set "FINAL_RESULT=1"
set "RETRY_TARGET=ASK_PORT"
goto ASK_RETRY

:SELF_TEST_PASS
echo.
echo SELF-TEST PASS - BIN hash, Python and esptool are ready.
>> "%LOG%" echo SELF-TEST PASS.
if "%PUSHD_OK%"=="1" popd >nul 2>&1
exit /b 0

:EXIT_TOOL
echo.
set "CLOSE="
set /p "CLOSE=Press ENTER to close this window."
if "%PUSHD_OK%"=="1" popd >nul 2>&1
exit /b !FINAL_RESULT!
