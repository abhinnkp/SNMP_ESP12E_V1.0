@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

set "TITLE_TEXT=ETPL SNMP Field Engineer Test Dashboard"
set "TOOL_VERSION=SNMP_TESTER_TIMING_20260929"
title %TITLE_TEXT%
set "ROOT=%~dp0"
pushd "%ROOT%" >nul 2>&1
if errorlevel 1 goto ERR_FOLDER

echo ============================================================
echo  %TITLE_TEXT%
echo ============================================================
echo  VERSION: %TOOL_VERSION%
echo  USE ONLY THIS FILE: TEST_SNMP_20260929.bat
echo ============================================================
echo.
echo SNMP config:
echo - Version: v2c
echo - Community: public
echo - UDP port: 161
echo - sysDescr OID: 1.3.6.1.2.1.1.1.0
echo - Inverter base OID: 1.3.6.1.4.1.12345.1.23
echo - Inverter values: .1 to .23
echo.

:ASK_TARGET
set "HOST="
set /p "HOST=Enter device IP or AUTO: "
if "%HOST%"=="" goto ASK_TARGET
set "COMMUNITY=public"
set "DASH_PORT=8765"
set "TIMEOUT=4.0"
set "RETRIES=1"
set "RUNTIME=%ROOT%_snmp_dashboard_runtime"
set "PYFILE=%RUNTIME%\snmp_dashboard.py"
set "SELF=%~f0"
set "INSTALLERS=%RUNTIME%\installers"
set "NETSNMP_LOCAL_DIR=%LOCALAPPDATA%\ETPL_Net_SNMP"
set "NETSNMP_X64_URL=https://downloads.sourceforge.net/project/net-snmp/net-snmp%%20binaries/5.5-binaries/net-snmp-5.5.0-2.x64.exe"
set "NETSNMP_X86_URL=https://downloads.sourceforge.net/project/net-snmp/net-snmp%%20binaries/5.5-binaries/net-snmp-5.5.0-1.x86.exe"

set "PYTHON="
set "PY312=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
set "PY311=%LOCALAPPDATA%\Programs\Python\Python311\python.exe"

if exist "%PY312%" (
  "%PY312%" -c "import sys" >nul 2>&1
  if not errorlevel 1 set "PYTHON=%PY312%"
)
if not defined PYTHON if exist "%PY311%" (
  "%PY311%" -c "import sys" >nul 2>&1
  if not errorlevel 1 set "PYTHON=%PY311%"
)
if not defined PYTHON (
  where py >nul 2>&1
  if not errorlevel 1 (
    for /f "delims=" %%P in ('py -3 -c "import sys; print(sys.executable)" 2^>nul') do set "PYTHON=%%P"
  )
)
if not defined PYTHON (
  where python >nul 2>&1
  if not errorlevel 1 (
    for /f "delims=" %%P in ('python -c "import sys; print(sys.executable)" 2^>nul') do set "PYTHON=%%P"
  )
)
if not defined PYTHON goto INSTALL_PYTHON
goto PY_READY

:INSTALL_PYTHON
echo Python was not found.
echo Trying automatic Python install through winget...
where winget >nul 2>&1
if errorlevel 1 goto ERR_PYTHON
winget install -e --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
if exist "%PY312%" (
  "%PY312%" -c "import sys" >nul 2>&1
  if not errorlevel 1 set "PYTHON=%PY312%"
)
if not defined PYTHON goto ERR_PYTHON

:PY_READY
echo Python:
echo   %PYTHON%
echo.

if not exist "%RUNTIME%" mkdir "%RUNTIME%" >nul 2>&1
if errorlevel 1 goto ERR_RUNTIME
if not exist "%INSTALLERS%" mkdir "%INSTALLERS%" >nul 2>&1

call :FIND_SNMPWALK
if defined SNMPWALK_PATH (
  echo snmpwalk.exe: %SNMPWALK_PATH%
  goto SNMPWALK_DONE
)

echo snmpwalk.exe: not found.
echo Trying automatic Net-SNMP download/install...
call :AUTO_INSTALL_SNMPWALK
call :FIND_SNMPWALK
if defined SNMPWALK_PATH (
  echo snmpwalk.exe installed/found:
  echo   %SNMPWALK_PATH%
) else (
  echo snmpwalk.exe: still not found. Built-in SNMP engine will be used.
)
:SNMPWALK_DONE
echo.

if not exist "%RUNTIME%" mkdir "%RUNTIME%" >nul 2>&1
if errorlevel 1 goto ERR_RUNTIME

"%PYTHON%" -c "import os,pathlib; text=pathlib.Path(os.environ['SELF']).read_text(encoding='utf-8-sig'); marker='###SNMP_DASHBOARD_PYTHON_START###'; source=text.rsplit(marker,1)[1].lstrip(); compile(source,os.environ['PYFILE'],'exec'); pathlib.Path(os.environ['PYFILE']).write_text(source,encoding='utf-8')"
if errorlevel 1 goto ERR_EXTRACT

echo Starting dashboard...
echo URL:
echo   http://127.0.0.1:%DASH_PORT%/?host=%HOST%
echo.
echo Keep this window open. Press Ctrl+C to stop dashboard.
echo Logs:
echo   %RUNTIME%\logs
echo.

"%PYTHON%" "%PYFILE%" --host "%HOST%" --community "%COMMUNITY%" --port %DASH_PORT% --timeout %TIMEOUT% --retries %RETRIES%
set "RESULT=%ERRORLEVEL%"
echo.
echo Dashboard stopped. Exit code: %RESULT%
echo Press ENTER to close.
set "CLOSE="
set /p "CLOSE="
exit /b %RESULT%

:FIND_SNMPWALK
set "SNMPWALK_PATH="
where snmpwalk.exe >nul 2>&1
if not errorlevel 1 (
  for /f "delims=" %%S in ('where snmpwalk.exe 2^>nul') do (
    set "SNMPWALK_PATH=%%S"
    goto FIND_SNMPWALK_END
  )
)
for %%D in ("%ROOT%tools\net-snmp\bin" "%ROOT%net-snmp\bin" "%RUNTIME%\net-snmp\bin" "%NETSNMP_LOCAL_DIR%\bin" "%ProgramFiles%\Net-SNMP\bin" "%ProgramFiles(x86)%\Net-SNMP\bin" "C:\usr\bin" "C:\net-snmp\bin") do (
  if exist "%%~D\snmpwalk.exe" (
    set "PATH=%%~D;%PATH%"
    set "SNMPWALK_PATH=%%~D\snmpwalk.exe"
    goto FIND_SNMPWALK_END
  )
)
:FIND_SNMPWALK_END
exit /b 0

:AUTO_INSTALL_SNMPWALK
where powershell >nul 2>&1
if errorlevel 1 (
  echo PowerShell not found, Net-SNMP auto-download skipped.
  exit /b 1
)
if not exist "%INSTALLERS%" mkdir "%INSTALLERS%" >nul 2>&1

set "NETSNMP_URL=%NETSNMP_X86_URL%"
set "NETSNMP_FILE=net-snmp-5.5.0-1.x86.exe"
if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
  set "NETSNMP_URL=%NETSNMP_X64_URL%"
  set "NETSNMP_FILE=net-snmp-5.5.0-2.x64.exe"
)
if /I "%PROCESSOR_ARCHITEW6432%"=="AMD64" (
  set "NETSNMP_URL=%NETSNMP_X64_URL%"
  set "NETSNMP_FILE=net-snmp-5.5.0-2.x64.exe"
)

set "NETSNMP_INSTALLER=%INSTALLERS%\%NETSNMP_FILE%"
if exist "%NETSNMP_INSTALLER%" (
  for %%A in ("%NETSNMP_INSTALLER%") do (
    if %%~zA LSS 1000000 (
      echo Removing incomplete old Net-SNMP download:
      echo   "%NETSNMP_INSTALLER%"
      del /f /q "%NETSNMP_INSTALLER%" >nul 2>&1
    )
  )
)
if not exist "%NETSNMP_INSTALLER%" (
  set "NETSNMP_PART=%NETSNMP_INSTALLER%.part"
  if exist "!NETSNMP_PART!" del /f /q "!NETSNMP_PART!" >nul 2>&1
  echo Downloading Net-SNMP:
  echo   %NETSNMP_URL%
  where curl.exe >nul 2>&1
  if not errorlevel 1 (
    curl.exe -L --http1.1 --no-keepalive --ssl-no-revoke --fail --retry 1 --retry-max-time 120 --connect-timeout 20 --max-time 120 --speed-time 30 --speed-limit 1000 -o "!NETSNMP_PART!" "%NETSNMP_URL%"
  ) else (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri $env:NETSNMP_URL -OutFile $env:NETSNMP_PART -UseBasicParsing"
  )
  if errorlevel 1 (
    echo Net-SNMP download failed.
    exit /b 1
  )
  if not exist "!NETSNMP_PART!" (
    echo Net-SNMP temporary download file was not created.
    exit /b 1
  )
  move /Y "!NETSNMP_PART!" "%NETSNMP_INSTALLER%" >nul 2>&1
  if errorlevel 1 (
    echo Could not finalize Net-SNMP download.
    exit /b 1
  )
)

if not exist "%NETSNMP_INSTALLER%" (
  echo Net-SNMP installer file was not created.
  exit /b 1
)

for %%A in ("%NETSNMP_INSTALLER%") do (
  if %%~zA LSS 1000000 (
    echo Net-SNMP download looks incomplete:
    echo   "%NETSNMP_INSTALLER%"
    del /f /q "%NETSNMP_INSTALLER%" >nul 2>&1
    exit /b 1
  )
)

echo Installing Net-SNMP silently...
echo   "%NETSNMP_INSTALLER%" /S /D=%NETSNMP_LOCAL_DIR%
"%NETSNMP_INSTALLER%" /S /D=%NETSNMP_LOCAL_DIR%
if errorlevel 1 (
  echo Net-SNMP installer returned an error.
  exit /b 1
)

if exist "%NETSNMP_LOCAL_DIR%\bin\snmpwalk.exe" set "PATH=%NETSNMP_LOCAL_DIR%\bin;%PATH%"
call :FIND_SNMPWALK
exit /b 0

:ERR_FOLDER
echo FAIL - Could not open script folder.
goto ERR_END

:ERR_PYTHON
echo FAIL - Python was not found and automatic install was not possible.
echo Install Python 3.12, then run this file again.
goto ERR_END

:ERR_RUNTIME
echo FAIL - Could not create runtime folder:
echo   %RUNTIME%
goto ERR_END

:ERR_EXTRACT
echo FAIL - Could not extract embedded dashboard script.
goto ERR_END

:ERR_END
echo.
echo Press ENTER to close.
set "CLOSE="
set /p "CLOSE="
exit /b 1

###SNMP_DASHBOARD_PYTHON_START###
from __future__ import annotations

import argparse
import concurrent.futures
import ipaddress
import json
import os
import random
import re
import shutil
import socket
import subprocess
import threading
import time
import webbrowser
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

VERSION = "SNMP_TESTER_TIMING_20260929"
BASE_OID = "1.3.6.1.4.1.12345.1.23"
SYS_DESCR_OID = "1.3.6.1.2.1.1.1.0"
DEFAULT_COMMUNITY = "public"
SNMP_PORT = 161
MAX_LEAF = 23
AUTO_HOST_TEXT = "AUTO"
COMMON_TARGETS: list[str] = []
POLL_INTERVAL = 7.0
POLL_BUDGET = 20.0
OID_GROUPS = [
    [SYS_DESCR_OID, BASE_OID + ".1", BASE_OID + ".23"],
    [f"{BASE_OID}.{leaf}" for leaf in range(2, 10)],
    [f"{BASE_OID}.{leaf}" for leaf in range(10, 18)],
    [f"{BASE_OID}.{leaf}" for leaf in range(18, 23)],
]

LABELS = {
    1: ("Device MAC", ""),
    2: ("Output Voltage", "V"),
    3: ("Output Frequency", "Hz"),
    4: ("Output Current", "A"),
    5: ("Output Power", "W"),
    6: ("Input Voltage", "V"),
    7: ("Input Frequency", "Hz"),
    8: ("Battery Voltage", "V"),
    9: ("Battery Charging Current", "A"),
    10: ("Solar Voltage", "V"),
    11: ("Solar Current", "A"),
    12: ("Solar Power", "W"),
    13: ("Output Energy", "Wh"),
    14: ("Solar Energy", "Wh"),
    15: ("Flash Writes", ""),
    16: ("Uptime", "min"),
    17: ("Temperature", "C"),
    18: ("Inverter Status Fault", ""),
    19: ("PFC Charger Status Fault", ""),
    20: ("MPPT Charger Status Fault", ""),
    21: ("Mains Grid Status Fault", ""),
    22: ("Battery Status Fault", ""),
    23: ("Full Packet", ""),
}

SCALE10 = {3, 4, 7, 8, 9, 10, 11, 17}


def now_text() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def ber_length(length: int) -> bytes:
    if length < 128:
        return bytes([length])
    raw = length.to_bytes((length.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(raw)]) + raw


def tlv(tag: int, value: bytes) -> bytes:
    return bytes([tag]) + ber_length(len(value)) + value


def integer(value: int) -> bytes:
    if value == 0:
        raw = b"\0"
    else:
        width = max(1, (value.bit_length() + 8) // 8)
        raw = value.to_bytes(width, "big", signed=True)
        while len(raw) > 1 and raw[0] == 0 and raw[1] < 0x80:
            raw = raw[1:]
    return tlv(0x02, raw)


def oid_bytes(oid: str) -> bytes:
    arcs = [int(part) for part in oid.strip(".").split(".")]
    if len(arcs) < 2 or arcs[0] > 2 or (arcs[0] < 2 and arcs[1] > 39):
        raise ValueError(f"Invalid OID: {oid}")
    values = [40 * arcs[0] + arcs[1], *arcs[2:]]
    encoded = bytearray()
    for value in values:
        groups = [value & 0x7F]
        value >>= 7
        while value:
            groups.append(0x80 | (value & 0x7F))
            value >>= 7
        encoded.extend(reversed(groups))
    return bytes(encoded)


def oid_text(raw: bytes) -> str:
    if not raw:
        return ""
    first = raw[0]
    arcs = [first // 40, first % 40] if first < 80 else [2, first - 80]
    value = 0
    for byte in raw[1:]:
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            arcs.append(value)
            value = 0
    if value:
        raise ValueError("Malformed OID in SNMP response")
    return ".".join(map(str, arcs))


def read_tlv(packet: bytes, offset: int) -> tuple[int, bytes, int]:
    if offset + 2 > len(packet):
        raise ValueError("Truncated SNMP response")
    tag = packet[offset]
    offset += 1
    first_length = packet[offset]
    offset += 1
    if first_length < 0x80:
        length = first_length
    else:
        count = first_length & 0x7F
        if count == 0 or count > 4 or offset + count > len(packet):
            raise ValueError("Invalid BER length")
        length = int.from_bytes(packet[offset:offset + count], "big")
        offset += count
    end = offset + length
    if end > len(packet):
        raise ValueError("Truncated SNMP value")
    return tag, packet[offset:end], end


def decode_integer(raw: bytes, signed: bool = False) -> int:
    return int.from_bytes(raw, "big", signed=signed)


def make_request(request_id: int, community: str, oid: str | list[str], pdu_tag: int) -> bytes:
    oids = [oid] if isinstance(oid, str) else oid
    varbinds = b"".join(tlv(0x30, tlv(0x06, oid_bytes(item)) + tlv(0x05, b"")) for item in oids)
    pdu = tlv(0x30, varbinds)
    pdu_body = integer(request_id) + integer(0) + integer(0) + pdu
    message = integer(1) + tlv(0x04, community.encode("utf-8")) + tlv(pdu_tag, pdu_body)
    return tlv(0x30, message)


def parse_response(packet: bytes, expected_request_id: int) -> list[tuple[str, int, bytes]]:
    outer_tag, outer, _ = read_tlv(packet, 0)
    if outer_tag != 0x30:
        raise ValueError("Not an SNMP sequence")
    _, _, pos = read_tlv(outer, 0)
    _, _, pos = read_tlv(outer, pos)
    pdu_tag, pdu, _ = read_tlv(outer, pos)
    if pdu_tag != 0xA2:
        raise ValueError(f"Expected SNMP response PDU, got 0x{pdu_tag:02x}")
    _, req_raw, pdu_pos = read_tlv(pdu, 0)
    request_id = decode_integer(req_raw, signed=True)
    if request_id != expected_request_id:
        raise ValueError("Received a response for a different request")
    _, err_raw, pdu_pos = read_tlv(pdu, pdu_pos)
    _, _, pdu_pos = read_tlv(pdu, pdu_pos)
    error_status = decode_integer(err_raw)
    if error_status:
        raise ValueError(f"SNMP agent error status {error_status}")
    seq_tag, varbind_list, _ = read_tlv(pdu, pdu_pos)
    if seq_tag != 0x30:
        raise ValueError("Invalid SNMP varbind list")
    values = []
    vb_pos = 0
    while vb_pos < len(varbind_list):
        vb_tag, varbind, vb_pos = read_tlv(varbind_list, vb_pos)
        if vb_tag != 0x30:
            raise ValueError("Invalid SNMP varbind")
        oid_tag, oid_raw, value_pos = read_tlv(varbind, 0)
        value_tag, value_raw, _ = read_tlv(varbind, value_pos)
        if oid_tag != 0x06:
            raise ValueError("Invalid SNMP response OID")
        values.append((oid_text(oid_raw), value_tag, value_raw))
    return values


def snmp_request(host: str, community: str, oid: str | list[str], pdu_tag: int, timeout: float, retries: int) -> list[tuple[str, int, bytes]]:
    last_error = "No response"
    for _ in range(retries + 1):
        request_id = random.randint(1, 0x7FFFFFFF)
        packet = make_request(request_id, community, oid, pdu_tag)
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(timeout)
                sock.connect((host, SNMP_PORT))
                sock.send(packet)
                response = sock.recv(4096)
            return parse_response(response, request_id)
        except (OSError, ValueError, socket.timeout) as exc:
            last_error = str(exc) or "No response"
    raise RuntimeError(last_error)


def is_auto_host(host: str) -> bool:
    return not host or host.strip().lower() in {"auto", "scan", "find", "*"}


def clean_host(host: str) -> str:
    host = (host or "").strip()
    return AUTO_HOST_TEXT if is_auto_host(host) else host


def is_good_ipv4(text: str) -> bool:
    try:
        ip = ipaddress.ip_address(text)
        return ip.version == 4 and not ip.is_loopback and not ip.is_multicast and not ip.is_unspecified
    except ValueError:
        return False


def local_ipv4_addresses() -> list[str]:
    found: set[str] = set()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            found.add(sock.getsockname()[0])
    except Exception:
        pass
    try:
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            found.add(item[4][0])
    except Exception:
        pass
    try:
        out = subprocess.run(["ipconfig"], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=3.0)
        for match in re.finditer(r"IPv4[^:\r\n]*:\s*([0-9]+(?:\.[0-9]+){3})", out.stdout):
            found.add(match.group(1))
    except Exception:
        pass
    return sorted(ip for ip in found if is_good_ipv4(ip) and not ip.startswith("169.254."))


_network_cache_lock = threading.Lock()
_network_cache: dict = {"time": 0.0, "adapters": [], "ssid": ""}
NETWORK_BACKUP_PATH = Path(os.environ.get("TEMP", ".")) / "ETPL_SNMP_network_backup.json"


def windows_network_details() -> tuple[list[dict], str]:
    if os.name != "nt":
        return [], ""
    with _network_cache_lock:
        if time.monotonic() - float(_network_cache["time"]) < 4.0:
            return list(_network_cache["adapters"]), str(_network_cache["ssid"])

    adapters: list[dict] = []
    ssid = ""
    script = r"""
$ErrorActionPreference='SilentlyContinue'
$rows = @(Get-NetAdapter | Where-Object {
  $_.InterfaceDescription -notmatch 'Loopback|Bluetooth'
} | ForEach-Object {
  $adapter = $_
  $addresses = @(Get-NetIPAddress -InterfaceIndex $adapter.InterfaceIndex -AddressFamily IPv4 | Where-Object {
    $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*'
  })
  if ($addresses.Count -eq 0) {
    [PSCustomObject]@{
      alias = $adapter.Name
      index = $adapter.InterfaceIndex
      ip = ''
      prefix = ''
      status = $adapter.Status
      description = $adapter.InterfaceDescription
    }
  } else {
    foreach ($address in $addresses) {
      [PSCustomObject]@{
        alias = $adapter.Name
        index = $adapter.InterfaceIndex
        ip = $address.IPAddress
        prefix = $address.PrefixLength
        status = $adapter.Status
        description = $adapter.InterfaceDescription
      }
    }
  }
})
$rows | ConvertTo-Json -Compress
"""
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=5.0,
        )
        if result.stdout.strip():
            parsed = json.loads(result.stdout.lstrip("\ufeff??? \r\n\t"))
            adapters = parsed if isinstance(parsed, list) else [parsed]
    except Exception:
        adapters = []
    try:
        result = subprocess.run(
            ["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True,
            encoding="utf-8", errors="ignore", timeout=3.0,
        )
        for line in result.stdout.splitlines():
            match = re.match(r"\s*SSID\s*:\s*(.+?)\s*$", line, re.IGNORECASE)
            if match:
                ssid = match.group(1)
                break
    except Exception:
        pass
    with _network_cache_lock:
        _network_cache.update({"time": time.monotonic(), "adapters": adapters, "ssid": ssid})
    return list(adapters), ssid


def route_source_ip(host: str) -> str:
    if is_auto_host(host) or not is_good_ipv4(host):
        return ""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect((host, SNMP_PORT))
            return sock.getsockname()[0]
    except OSError:
        return ""


def network_context(host: str) -> dict:
    adapters, ssid = windows_network_details()
    source_ip = route_source_ip(host)
    selected = next((row for row in adapters if str(row.get("ip", "")) == source_ip), {})
    alias = str(selected.get("alias", ""))
    prefix = selected.get("prefix", "")
    route_text = f"{alias}: {source_ip}/{prefix}" if alias else (source_ip or "No IPv4 route")
    adapter_text = "; ".join(
        f"{row.get('alias', '?')}: "
        f"{(str(row.get('ip', '')) + '/' + str(row.get('prefix', ''))) if row.get('ip') else 'no IPv4'} "
        f"({row.get('status', '?')})"
        for row in adapters
    ) or ", ".join(local_ipv4_addresses()) or "No active IPv4 adapter"
    warning = ""
    is_wifi_route = "wi-fi" in alias.lower() or "wireless" in alias.lower()
    if ssid.lower().startswith("invertersetup_") and (is_wifi_route or not alias):
        warning = (
            f"Windows is routing through setup hotspot {ssid}. The hotspot is configuration-only; "
            "connect/configure the wired Ethernet adapter for the device IP subnet."
        )
    elif is_wifi_route:
        warning = (
            f"Target route uses Wi-Fi ({ssid or alias}). This works only when the controller ENC28J60 "
            "is connected to that same router/LAN; for direct cable the route must use Ethernet."
        )
    elif alias:
        warning = f"Target route uses {alias} with source {source_ip}/{prefix}."
    return {
        "routeSource": source_ip,
        "routeAdapter": alias,
        "routeText": route_text,
        "adapterText": adapter_text,
        "wifiSsid": ssid or "Not connected",
        "routeWarning": warning,
    }


def _powershell_json(script: str, extra_env: dict[str, str] | None = None) -> dict:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    result = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30.0, env=env,
    )
    output = result.stdout.strip().lstrip("\ufeff??? \r\n\t")
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "PowerShell network command failed").strip()
        raise RuntimeError(detail)
    if not output:
        raise RuntimeError("PowerShell network command returned no result")
    return json.loads(output)


def fix_ethernet_route(host: str) -> dict:
    host = clean_host(host)
    if is_auto_host(host) or not is_good_ipv4(host):
        raise ValueError("Enter the device IPv4 address before fixing the Ethernet route.")
    script = r"""
$ErrorActionPreference='Stop'
$target = [Net.IPAddress]::Parse($env:ETPL_TARGET_IP)
$wired = @(Get-NetAdapter | Where-Object {
  $_.Name -notmatch 'Wi-Fi|Wireless|WLAN|Bluetooth|vEthernet' -and
  $_.InterfaceDescription -notmatch 'Wi-Fi|Wireless|WLAN|Bluetooth|Virtual|VPN|Loopback'
} | Sort-Object @{Expression={if ($_.Status -eq 'Up') {0} else {1}}}, InterfaceIndex | Select-Object -First 1)
if ($wired.Count -eq 0) { throw 'No wired Ethernet adapter was found in Windows.' }
$wired = $wired[0]
if ($wired.Status -eq 'Disabled') {
  Enable-NetAdapter -InterfaceIndex $wired.InterfaceIndex -Confirm:$false
  Start-Sleep -Seconds 2
  $wired = Get-NetAdapter -InterfaceIndex $wired.InterfaceIndex
}
if ($wired.Status -ne 'Up') {
  throw ("Ethernet adapter '{0}' is {1}. Connect the cable and confirm both RJ45 link LEDs are ON." -f $wired.Name,$wired.Status)
}
$ipInterface = Get-NetIPInterface -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4
$oldAddresses = @(Get-NetIPAddress -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 | Where-Object {
  $_.IPAddress -notlike '169.254.*'
} | ForEach-Object { [PSCustomObject]@{ip=$_.IPAddress;prefix=$_.PrefixLength} })
$wifiUp = @(Get-NetAdapter | Where-Object {
  ($_.Name -match 'Wi-Fi|Wireless|WLAN' -or $_.InterfaceDescription -match 'Wi-Fi|Wireless|WLAN') -and $_.Status -eq 'Up'
} | Select-Object -ExpandProperty InterfaceIndex)
$backup = [PSCustomObject]@{
  wiredIndex=$wired.InterfaceIndex
  wiredName=$wired.Name
  dhcp=[string]$ipInterface.Dhcp
  metric=$ipInterface.InterfaceMetric
  addresses=$oldAddresses
  wifiUp=$wifiUp
}
try {
  foreach ($wifiIndex in $wifiUp) { Disable-NetAdapter -InterfaceIndex $wifiIndex -Confirm:$false }
  $octets = $env:ETPL_TARGET_IP.Split('.')
  $last = 112
  if ([int]$octets[3] -eq $last) { $last = 113 }
  $pcIP = "{0}.{1}.{2}.{3}" -f $octets[0],$octets[1],$octets[2],$last
  Set-NetIPInterface -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -Dhcp Disabled -InterfaceMetric 5
  Get-NetIPAddress -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
    $_.PrefixOrigin -ne 'WellKnown'
  } | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
  New-NetIPAddress -InterfaceIndex $wired.InterfaceIndex -IPAddress $pcIP -PrefixLength 24 -AddressFamily IPv4 | Out-Null
  Start-Sleep -Seconds 2
  [PSCustomObject]@{
    ok=$true
    message=("Ethernet route fixed: {0} = {1}/24; Wi-Fi disabled." -f $wired.Name,$pcIP)
    adapter=$wired.Name
    pcIP=$pcIP
    target=$env:ETPL_TARGET_IP
    backup=$backup
  } | ConvertTo-Json -Compress -Depth 6
} catch {
  Get-NetIPAddress -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
    $_.PrefixOrigin -ne 'WellKnown'
  } | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
  if ([string]$backup.dhcp -eq 'Enabled') {
    Set-NetIPInterface -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -Dhcp Enabled -InterfaceMetric ([int]$backup.metric)
  } else {
    Set-NetIPInterface -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -Dhcp Disabled -InterfaceMetric ([int]$backup.metric)
    foreach ($address in @($backup.addresses)) {
      New-NetIPAddress -InterfaceIndex $wired.InterfaceIndex -IPAddress $address.ip -PrefixLength ([int]$address.prefix) -AddressFamily IPv4 | Out-Null
    }
  }
  foreach ($wifiIndex in $wifiUp) { Enable-NetAdapter -InterfaceIndex $wifiIndex -Confirm:$false -ErrorAction SilentlyContinue }
  throw ("Ethernet route fix failed; previous network was restored. " + $_.Exception.Message)
}
"""
    result = _powershell_json(script, {"ETPL_TARGET_IP": host})
    NETWORK_BACKUP_PATH.write_text(json.dumps(result.get("backup", {})), encoding="utf-8")
    with _network_cache_lock:
        _network_cache["time"] = 0.0
    return result


def restore_pc_network() -> dict:
    if not NETWORK_BACKUP_PATH.exists():
        raise RuntimeError("No ETPL network backup was found. Nothing has been changed by this tester session.")
    backup_text = NETWORK_BACKUP_PATH.read_text(encoding="utf-8")
    script = r"""
$ErrorActionPreference='Stop'
$backup = $env:ETPL_NETWORK_BACKUP | ConvertFrom-Json
$wired = Get-NetAdapter -InterfaceIndex ([int]$backup.wiredIndex)
if ($wired.Status -eq 'Disabled') { Enable-NetAdapter -InterfaceIndex $wired.InterfaceIndex -Confirm:$false }
Get-NetIPAddress -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
  $_.PrefixOrigin -ne 'WellKnown'
} | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
if ([string]$backup.dhcp -eq 'Enabled') {
  Set-NetIPInterface -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -Dhcp Enabled -InterfaceMetric ([int]$backup.metric)
} else {
  Set-NetIPInterface -InterfaceIndex $wired.InterfaceIndex -AddressFamily IPv4 -Dhcp Disabled -InterfaceMetric ([int]$backup.metric)
  foreach ($address in @($backup.addresses)) {
    New-NetIPAddress -InterfaceIndex $wired.InterfaceIndex -IPAddress $address.ip -PrefixLength ([int]$address.prefix) -AddressFamily IPv4 | Out-Null
  }
}
foreach ($wifiIndex in @($backup.wifiUp)) {
  Enable-NetAdapter -InterfaceIndex ([int]$wifiIndex) -Confirm:$false -ErrorAction SilentlyContinue
}
[PSCustomObject]@{ok=$true;message='Previous Ethernet configuration and Wi-Fi state restored.'} | ConvertTo-Json -Compress
"""
    result = _powershell_json(script, {"ETPL_NETWORK_BACKUP": backup_text})
    with _network_cache_lock:
        _network_cache["time"] = 0.0
    return result


def same_24(ip: str) -> list[str]:
    try:
        addr = ipaddress.ip_address(ip)
        if addr.version != 4:
            return []
        network = ipaddress.ip_network(f"{ip}/24", strict=False)
        if not network.is_private:
            return []
        return [str(host) for host in network.hosts() if str(host) != ip]
    except ValueError:
        return []


def discovery_candidates(preferred: str = "") -> list[str]:
    seen: set[str] = set()
    result: list[str] = []

    def add(ip: str):
        if is_good_ipv4(ip) and ip not in seen:
            seen.add(ip)
            result.append(ip)

    if preferred and not is_auto_host(preferred):
        add(preferred)
    for ip in COMMON_TARGETS:
        add(ip)
    locals_ = local_ipv4_addresses()
    for local_ip in locals_:
        third_octet = local_ip.split(".")[:3]
        for last in ("101", "111", "51", "1"):
            add(".".join(third_octet + [last]))
    for local_ip in locals_:
        for ip in same_24(local_ip):
            add(ip)
    adapters, _ = windows_network_details()
    for adapter in adapters:
        try:
            network = ipaddress.ip_network(f"{adapter['ip']}/{adapter['prefix']}", strict=False)
            if adapter.get("status") == "Up" and network.num_addresses <= 1024:
                for ip in network.hosts():
                    if str(ip) not in locals_:
                        add(str(ip))
        except (ValueError, KeyError):
            continue
    return result


def quick_etpl_probe(host: str, community: str, timeout: float = 4.0) -> dict | None:
    response = snmp_request(host, community, [SYS_DESCR_OID, BASE_OID + ".1"], 0xA0, timeout, 0)
    if len(response) != 2 or response[0][0] != SYS_DESCR_OID:
        return None
    _, sys_tag, sys_raw = response[0]
    _, sys_descr = format_snmp_value(sys_tag, sys_raw)
    first = response[1]
    oid, tag, _ = first
    if tag in (0x80, 0x81, 0x82) or not oid.startswith(BASE_OID + "."):
        return None
    return {"host": host, "sysDescr": str(sys_descr), "oid": oid}


def auto_discover_host(community: str, preferred: str = "", cancel=None) -> dict:
    candidates = discovery_candidates(preferred)
    if not candidates:
        raise RuntimeError("No local IPv4 adapter found for auto scan.")
    errors: list[str] = []
    workers = min(32, max(1, len(candidates)))
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=workers)
    try:
        futures = {executor.submit(quick_etpl_probe, host, community): host for host in candidates}
        for future in concurrent.futures.as_completed(futures):
            if cancel is not None and cancel.is_set():
                raise PollCancelled()
            host = futures[future]
            try:
                result = future.result()
                if result:
                    return {
                        "host": result["host"],
                        "sysDescr": result.get("sysDescr", ""),
                        "scanned": len(candidates),
                        "localIps": local_ipv4_addresses(),
                    }
            except Exception as exc:
                if len(errors) < 5:
                    errors.append(f"{host}: {str(exc) or exc.__class__.__name__}")
    finally:
        executor.shutdown(wait=False, cancel_futures=True)
    local_text = ", ".join(local_ipv4_addresses()) or "none"
    raise RuntimeError(
        f"Auto scan found no ETPL SNMP device. Scanned {len(candidates)} IPs. "
        f"PC IPv4: {local_text}. Connect the PC Ethernet adapter to the device LAN and give it an address in the same subnet."
    )


def format_snmp_value(tag: int, raw: bytes) -> tuple[str, str | int]:
    if tag == 0x04:
        return "STRING", raw.decode("utf-8", errors="replace")
    if tag == 0x02:
        return "INTEGER", decode_integer(raw, signed=True)
    if tag == 0x41:
        return "Counter32", decode_integer(raw)
    if tag == 0x42:
        return "Gauge32", decode_integer(raw)
    if tag == 0x43:
        return "Timeticks", decode_integer(raw)
    if tag == 0x46:
        return "Counter64", decode_integer(raw)
    if tag == 0x80:
        return "NoSuchObject", "No such object"
    if tag == 0x81:
        return "NoSuchInstance", "No such instance"
    if tag == 0x82:
        return "EndOfMibView", "End of MIB view"
    if tag == 0x05:
        return "NULL", ""
    return f"BER 0x{tag:02x}", raw.hex()


def leaf_from_oid(oid: str) -> int | None:
    prefix = BASE_OID + "."
    if not oid.startswith(prefix):
        return None
    suffix = oid[len(prefix):]
    if "." in suffix:
        return None
    try:
        leaf = int(suffix)
    except ValueError:
        return None
    return leaf if 1 <= leaf <= MAX_LEAF else None


def display_for_leaf(leaf: int, value):
    unit = LABELS.get(leaf, ("", ""))[1]
    if isinstance(value, int):
        if leaf in SCALE10:
            return f"{value / 10.0:.1f} {unit}".strip()
        return f"{value} {unit}".strip()
    return str(value)


def rows_from_values(values: list[tuple[str, int, bytes]]) -> list[dict]:
    rows = []
    seen = set()
    for oid, tag, raw in values:
        leaf = leaf_from_oid(oid)
        if not leaf or leaf in seen:
            continue
        seen.add(leaf)
        value_type, raw_value = format_snmp_value(tag, raw)
        label, unit = LABELS.get(leaf, (f"OID {leaf}", ""))
        rows.append({
            "leaf": leaf,
            "oid": oid,
            "label": label,
            "type": value_type,
            "raw": raw_value,
            "value": display_for_leaf(leaf, raw_value),
            "unit": unit,
        })
    rows.sort(key=lambda row: row["leaf"])
    return rows


class ToolUnavailable(RuntimeError):
    pass


class PollCancelled(RuntimeError):
    pass


def request_timeout(deadline: float, timeout: float, retries: int, cancel=None) -> float:
    if cancel is not None and cancel.is_set():
        raise PollCancelled()
    remaining = deadline - time.monotonic()
    if remaining < 0.25:
        raise TimeoutError("SNMP poll time budget exceeded")
    return min(timeout, remaining / (retries + 1))


def poll_groups(read_group, engine: str, progress=None) -> dict:
    rows = {}
    sys_descr = ""
    for group in OID_GROUPS:
        description, group_rows = read_group(group)
        if description:
            sys_descr = description
        rows.update({row["leaf"]: row for row in group_rows})
        result = {"engine": engine, "sysDescr": sys_descr,
                  "rows": sorted(rows.values(), key=lambda row: row["leaf"])}
        if progress:
            progress(result)
        expected = {leaf_from_oid(oid) for oid in group if oid != SYS_DESCR_OID}
        if not expected.issubset({row["leaf"] for row in group_rows}):
            raise RuntimeError("Incomplete SNMP GET response: requested OIDs are missing")
    return result


def builtin_poll(host: str, community: str, timeout: float, retries: int, progress=None, cancel=None) -> dict:
    deadline = time.monotonic() + POLL_BUDGET

    def read_group(group):
        wait = request_timeout(deadline, timeout, retries, cancel)
        values = snmp_request(host, community, group, 0xA0, wait, retries)
        if {oid for oid, _, _ in values} != set(group):
            raise RuntimeError("SNMP response OIDs do not match the request")
        if any(tag in (0x80, 0x81, 0x82, 0x05) for _, tag, _ in values):
            raise RuntimeError("Device did not return the requested ETPL OIDs")
        description = next((str(format_snmp_value(tag, raw)[1]) for oid, tag, raw in values
                            if oid == SYS_DESCR_OID), "")
        return description, rows_from_values(values)

    return poll_groups(read_group, "built-in SNMP v2c grouped GET", progress)


def parse_net_snmp_lines(text: str) -> list[tuple[str, str]]:
    parsed = []
    for line in text.splitlines():
        match = re.match(r"\s*\.?([0-9]+(?:\.[0-9]+)+)\s*=\s*(.+?)\s*$", line)
        if match:
            parsed.append((match.group(1), match.group(2)))
    return parsed


def net_value_to_row(oid: str, text: str) -> dict | None:
    leaf = leaf_from_oid(oid)
    if not leaf:
        return None
    lower_text = text.lower()
    if "no more variables" in lower_text or "end of mib" in lower_text or "no such" in lower_text:
        return None
    label, unit = LABELS.get(leaf, (f"OID {leaf}", ""))
    if ":" in text:
        value_type, value_text = text.split(":", 1)
        value_type = value_type.strip()
        value_text = value_text.strip().strip('"')
    else:
        value_type, value_text = "VALUE", text.strip().strip('"')
    raw_value = value_text
    if re.fullmatch(r"-?\d+", value_text):
        raw_value = int(value_text)
    elif value_type.lower() == "timeticks":
        match = re.match(r"\((\d+)\)", value_text)
        if match:
            raw_value = int(match.group(1))
    return {
        "leaf": leaf,
        "oid": oid,
        "label": label,
        "type": value_type,
        "raw": raw_value,
        "value": display_for_leaf(leaf, raw_value),
        "unit": unit,
    }


def net_snmp_poll(host: str, community: str, timeout: float, retries: int, progress=None, cancel=None) -> dict:
    snmpget = shutil.which("snmpget.exe") or shutil.which("snmpget")
    if not snmpget:
        raise ToolUnavailable("snmpget.exe not found; using built-in SNMP")
    deadline = time.monotonic() + POLL_BUDGET

    def read_group(group):
        wait = request_timeout(deadline, timeout, retries, cancel)
        command = [snmpget, "-v", "2c", "-c", community, "-t", str(wait), "-r", str(retries),
                   "-On", f"{host}:{SNMP_PORT}", *group]
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=max(0.25, deadline - time.monotonic()),
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except OSError as exc:
            raise ToolUnavailable(str(exc)) from exc
        if result.returncode != 0:
            raise RuntimeError((result.stderr or result.stdout or "SNMP GET failed").strip())
        parsed = parse_net_snmp_lines(result.stdout)
        if {oid for oid, _ in parsed} != set(group):
            raise RuntimeError("SNMP response OIDs do not match the request")
        description = next((text.split(":", 1)[-1].strip().strip('"') for oid, text in parsed
                            if oid == SYS_DESCR_OID), "")
        rows = [row for oid, text in parsed if (row := net_value_to_row(oid, text))]
        return description, rows

    return poll_groups(read_group, "Net-SNMP grouped GET", progress)


def snmpwalk_path() -> str:
    return shutil.which("snmpwalk.exe") or shutil.which("snmpwalk") or ""


def engine_note_text(engine_error: str) -> str:
    if not engine_error:
        return ""
    lower = engine_error.lower()
    if "not found" in lower and "snmp" in lower:
        return "snmpwalk.exe/snmpget.exe not found; built-in SNMP engine was used."
    return "snmpwalk fallback: " + engine_error


def ping_hint(host: str) -> str:
    if is_auto_host(host):
        return "Auto scan sends direct SNMP probes; ping is skipped for AUTO."
    try:
        if os.name == "nt":
            cmd = ["ping", "-n", "1", "-w", "900", host]
        else:
            cmd = ["ping", "-c", "1", "-W", "1", host]
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=2.0)
        text = (out.stdout + "\n" + out.stderr).strip()
        for line in text.splitlines():
            line = line.strip()
            if "Destination host unreachable" in line:
                return "Ping says Destination host unreachable; IP is not present on this LAN/subnet."
            if "Request timed out" in line:
                return "Ping request timed out; device did not answer ICMP."
            if line.lower().startswith("reply from") and "ttl=" in line.lower():
                return "Ping OK."
        return "Ping also failed/no reply."
    except Exception:
        return "Ping check not available."


def friendly_poll_error(host: str, exc: Exception, engine_error: str) -> tuple[str, str]:
    message = str(exc) or exc.__class__.__name__
    note = engine_note_text(engine_error)
    lower = message.lower()
    if is_auto_host(host):
        return message, note
    if isinstance(exc, TimeoutError) or "timed out" in lower or "timeout" in lower:
        message = f"SNMP timeout: no reply from {host}:{SNMP_PORT}"
        extra = ping_hint(host) + " SNMP is available on Ethernet, not on the setup hotspot. Check the PC Ethernet adapter is in the device subnet, the cable/link LEDs, firmware, and UDP 161."
        note = (note + " " + extra).strip()
    route_warning = network_context(host).get("routeWarning", "")
    if route_warning:
        note = (note + " " + route_warning).strip()
    return message, note


class Poller:
    def __init__(self, host: str, community: str, timeout: float, retries: int, logs_dir: Path):
        self.host = clean_host(host)
        self.community = community
        self.timeout = max(0.1, timeout)
        self.retries = max(0, min(2, retries))
        self.logs_dir = logs_dir
        self.logs_dir.mkdir(parents=True, exist_ok=True)
        self.log_path = self.logs_dir / ("snmp_dashboard_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".jsonl")
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.force_event = threading.Event()
        self.network_event = threading.Event()
        self.cancel_event = threading.Event()
        self.generation = 0
        self.walk_path = snmpwalk_path()
        self.network_state = {}
        self.state = self.empty_state()

    def empty_state(self):
        return {
            "version": VERSION, "ok": False, "snmpOnline": False,
            "host": self.host, "community": self.community, "snmpPort": SNMP_PORT,
            "baseOid": BASE_OID, "sysDescrOid": SYS_DESCR_OID,
            "status": "auto scanning" if is_auto_host(self.host) else "starting",
            "error": "", "engine": "", "engineNote": "", "snmpwalk": self.walk_path,
            "localIps": [], "updatedAt": "", "lastDataAt": "", "responseMs": None,
            "rows": [], "receivedCount": 0, "polling": False, "stale": False,
            "inverterStatus": "waiting",
        }

    def set_target(self, host: str, community: str):
        host = clean_host(host)
        community = community.strip() or DEFAULT_COMMUNITY
        if not is_auto_host(host):
            ipaddress.IPv4Address(host)
        with self.lock:
            if (host, community) != (self.host, self.community):
                self.cancel_event.set()
                self.cancel_event = threading.Event()
                self.generation += 1
                self.host, self.community = host, community
                self.state = self.empty_state()
                self.network_state = {}
                self.network_event.set()
        self.force_event.set()

    def snapshot(self) -> dict:
        with self.lock:
            return json.loads(json.dumps({**self.state, **self.network_state}))

    def write_log(self, record: dict):
        try:
            with self.log_path.open("a", encoding="utf-8") as handle:
                handle.write(json.dumps(record, ensure_ascii=False) + "\n")
        except OSError as exc:
            with self.lock:
                self.state["logWarning"] = str(exc)

    def poll_device(self, host, community, progress, cancel):
        try:
            return net_snmp_poll(host, community, self.timeout, self.retries, progress, cancel)
        except ToolUnavailable:
            # Only a missing/unusable executable selects the other engine.
            return builtin_poll(host, community, self.timeout, self.retries, progress, cancel)

    @staticmethod
    def inverter_status(rows):
        packet = next((str(row["raw"]).strip() for row in rows if row["leaf"] == 23), "")
        if packet == "$NO_DATA#":
            return "NO_DATA"
        if packet.startswith("$") and packet.endswith("#") and "," in packet:
            return "available (device cache)"
        return "unknown"

    def poll_once(self):
        with self.lock:
            host, community = self.host, self.community
            generation, cancel = self.generation, self.cancel_event
            self.state["polling"] = True
            self.state["receivedCount"] = 0
        started = time.monotonic()
        received = False

        def progress(result):
            nonlocal received
            received = True
            with self.lock:
                if generation != self.generation:
                    raise PollCancelled()
                self.state.update({
                    "snmpOnline": True, "engine": result["engine"],
                    "sysDescr": result["sysDescr"], "receivedCount": len(result["rows"]),
                    "inverterStatus": self.inverter_status(result["rows"]),
                    "responseMs": int((time.monotonic() - started) * 1000),
                })
                if not self.state["lastDataAt"]:
                    self.state["rows"] = result["rows"]
                    self.state["status"] = "reading OIDs"

        try:
            if is_auto_host(host):
                discovery = auto_discover_host(community, cancel=cancel)
                host = discovery["host"]
                with self.lock:
                    if generation != self.generation or cancel.is_set():
                        raise PollCancelled()
                    self.host = host
                    self.state["host"] = host
                    self.network_event.set()
            result = self.poll_device(host, community, progress, cancel)
            with self.lock:
                if generation != self.generation or cancel.is_set():
                    raise PollCancelled()
                inverter = self.inverter_status(result["rows"])
                self.state.update({
                    "ok": True, "snmpOnline": True, "host": host,
                    "status": "online" if inverter.startswith("available") else "inverter " + inverter,
                    "error": "", "engine": result["engine"], "engineNote": "",
                    "updatedAt": now_text(), "lastDataAt": now_text(),
                    "responseMs": int((time.monotonic() - started) * 1000),
                    "sysDescr": result["sysDescr"], "rows": result["rows"],
                    "receivedCount": len(result["rows"]), "polling": False, "stale": False,
                    "inverterStatus": inverter,
                })
                record = dict(self.state)
        except PollCancelled:
            return
        except Exception as exc:
            with self.lock:
                if generation != self.generation:
                    return
                stale = bool(self.state["rows"])
                self.state.update({
                    "ok": False, "snmpOnline": received, "polling": False, "stale": stale,
                    "status": "partial SNMP response" if received else "SNMP timeout/error",
                    "error": str(exc) or exc.__class__.__name__,
                    "engineNote": "Previous/incomplete values retained; not a fresh reading." if stale else "",
                    "updatedAt": now_text(),
                    "responseMs": int((time.monotonic() - started) * 1000),
                    "inverterStatus": "stale / not confirmed" if stale else "not confirmed",
                })
                record = dict(self.state)
        self.write_log(record)

    def run(self):
        while not self.stop_event.is_set():
            self.force_event.clear()
            started = time.monotonic()
            try:
                self.poll_once()
            except Exception as exc:
                with self.lock:
                    self.state.update({"ok": False, "polling": False, "status": "tester error",
                                       "error": str(exc), "stale": bool(self.state["rows"])})
            # Start-to-start interval; polls cannot overlap.
            self.force_event.wait(max(1.0, POLL_INTERVAL - (time.monotonic() - started)))

    def refresh_network_context(self):
        with self.lock:
            host, generation = self.host, self.generation
        details = network_context(host)
        details["localIps"] = local_ipv4_addresses()
        with self.lock:
            if generation == self.generation and host == self.host:
                self.network_state = details

    def network_loop(self):
        while not self.stop_event.is_set():
            self.network_event.clear()
            try:
                self.refresh_network_context()
            except Exception:
                pass
            self.network_event.wait(30.0)

    def stop(self):
        self.stop_event.set()
        self.cancel_event.set()
        self.force_event.set()
        self.network_event.set()


HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ETPL SNMP Dashboard</title>
<style>
body{margin:0;background:#0a0f18;color:#e8eef8;font:14px Arial,sans-serif}
.top{padding:18px 22px;background:#111b2b;border-bottom:1px solid #24344f;display:flex;gap:16px;align-items:center;justify-content:space-between;flex-wrap:wrap}
h1{font-size:20px;margin:0}.muted{color:#95a7c0}.wrap{padding:18px;max-width:1180px;margin:auto}
.bar{display:grid;grid-template-columns:repeat(4,minmax(140px,1fr));gap:10px;margin-bottom:14px}
.card{background:#101827;border:1px solid #22324b;border-radius:8px;padding:12px}.label{color:#9eb0c8;font-size:12px}.value{font-size:18px;font-weight:bold;margin-top:4px;word-break:break-word}
.ok{color:#46e6a7}.bad{color:#ff7777}.warn{color:#ffd166}
.controls{display:flex;gap:8px;flex-wrap:wrap;align-items:end;margin-bottom:14px;background:#101827;border:1px solid #22324b;border-radius:8px;padding:12px}
label{display:block;color:#9eb0c8;font-size:12px;margin-bottom:5px}input{background:#07101d;color:#e8eef8;border:1px solid #314762;border-radius:6px;padding:9px;min-width:160px}
button{background:#2cc9a6;color:#031014;border:0;border-radius:6px;padding:10px 14px;font-weight:bold;cursor:pointer}
.routefix{background:#ffd166}.restore{background:#32445d;color:#edf5ff}
.tablewrap{overflow-x:auto}table{width:100%;border-collapse:collapse;background:#101827;border:1px solid #22324b;border-radius:8px;overflow:hidden}
th,td{padding:9px 10px;border-bottom:1px solid #22324b;text-align:left;vertical-align:top}th{background:#152238;color:#b8c7db;font-size:12px}
tr:nth-child(even) td{background:#0d1523}.full{font-family:Consolas,monospace;font-size:12px;word-break:break-all}.small{font-size:12px}.error{background:#311621;border:1px solid #79364a;color:#ffd6de;border-radius:8px;padding:12px;margin-bottom:14px}.info{background:#122c31;border:1px solid #23606a;color:#cffbff;border-radius:8px;padding:12px;margin-bottom:14px}
@media(max-width:760px){.bar{grid-template-columns:1fr}.top{align-items:flex-start}.controls{display:block}.controls>div{margin-bottom:10px}}
</style>
</head>
<body>
<div class="top">
  <div><h1>ETPL SNMP Field Engineer Dashboard</h1><div class="muted small">v2c / community public / UDP 161 / base 1.3.6.1.4.1.12345.1.23</div></div>
  <div class="muted small" id="version"></div>
</div>
<div class="wrap">
  <div class="controls">
    <div><label>Device IP / AUTO</label><input id="host" value="" placeholder="AUTO"></div>
    <div><label>Community</label><input id="community" value="public"></div>
    <button onclick="applyTarget()">Apply / Refresh</button>
    <button onclick="autoDetect()">Auto Detect</button>
    <button class="routefix" onclick="fixEthernetRoute()">Fix Ethernet Route</button>
    <button class="restore" onclick="restoreNetwork()">Restore Network</button>
    <div class="muted small" id="pollstate">Starting</div>
  </div>
  <div id="error"></div>
  <div id="note"></div>
  <div class="bar">
    <div class="card"><div class="label">SNMP Status</div><div class="value" id="status">Starting</div></div>
    <div class="card"><div class="label">Target</div><div class="value" id="target">-</div></div>
    <div class="card"><div class="label">Response</div><div class="value" id="response">-</div></div>
    <div class="card"><div class="label">SNMP Engine</div><div class="value" id="engine">-</div></div>
  </div>
  <div class="bar">
    <div class="card"><div class="label">sysDescr</div><div class="value small" id="sysdescr">-</div></div>
    <div class="card"><div class="label">Last Complete Read</div><div class="value small" id="updated">-</div></div>
    <div class="card"><div class="label">snmpwalk.exe</div><div class="value small" id="snmpwalk">-</div></div>
    <div class="card"><div class="label">OID Count</div><div class="value" id="count">0 / 23</div></div>
    <div class="card"><div class="label">PC IPv4</div><div class="value small" id="localips">-</div></div>
    <div class="card"><div class="label">Inverter Data</div><div class="value small" id="inverter">Waiting</div></div>
  </div>
  <div class="bar">
    <div class="card"><div class="label">Selected Route</div><div class="value small" id="route">-</div></div>
    <div class="card"><div class="label">Windows Adapters</div><div class="value small" id="adapters">-</div></div>
    <div class="card"><div class="label">Connected Wi-Fi</div><div class="value small" id="ssid">-</div></div>
  </div>
  <div class="tablewrap"><table>
    <thead><tr><th>#</th><th>Name</th><th>Value</th><th>Raw</th><th>Type</th><th>OID</th></tr></thead>
    <tbody id="rows"><tr><td colspan="6">Waiting for data...</td></tr></tbody>
  </table></div>
</div>
<script>
const params = new URLSearchParams(location.search);
document.getElementById('host').value = params.get('host') || 'AUTO';
async function applyTarget(){
  const host = document.getElementById('host').value.trim();
  const community = document.getElementById('community').value.trim() || 'public';
  try {
    const response = await fetch('/api/set?host=' + encodeURIComponent(host) + '&community=' + encodeURIComponent(community));
    const result = await response.json();
    if(!response.ok) throw new Error(result.error || 'Target update failed');
    await refresh();
  } catch(e) { document.getElementById('error').textContent = e.message; }
}
async function autoDetect(){
  document.getElementById('host').value = 'AUTO';
  await applyTarget();
}
async function fixEthernetRoute(){
  const host = document.getElementById('host').value.trim();
  if(!host || host.toUpperCase()==='AUTO') { alert('Enter the device IP first.'); return; }
  if(!confirm('Configure wired Ethernet for ' + host + ' and temporarily disable Wi-Fi?')) return;
  try{
    const response = await fetch('/api/fix-route?host=' + encodeURIComponent(host), {method:'POST'});
    const result = await response.json();
    if(!response.ok || !result.ok) throw new Error(result.error || 'Route fix failed');
    alert(result.message);
    await applyTarget();
  }catch(e){ alert('Ethernet route was not changed: ' + e.message); }
}
async function restoreNetwork(){
  if(!confirm('Restore the Ethernet settings and Wi-Fi state saved before route fix?')) return;
  try{
    const response = await fetch('/api/restore-network', {method:'POST'});
    const result = await response.json();
    if(!response.ok || !result.ok) throw new Error(result.error || 'Restore failed');
    alert(result.message);
    await refresh();
  }catch(e){ alert('Network restore failed: ' + e.message); }
}
function esc(v){return String(v ?? '').replace(/[&<>"']/g, s => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[s]));}
let refreshing = false;
async function refresh(){
  if(refreshing) return;
  refreshing = true;
  try{
    const data = await (await fetch('/api/state?ts=' + Date.now())).json();
    document.getElementById('version').textContent = data.version || '';
    document.getElementById('status').innerHTML = data.snmpOnline ? '<span class="ok">RESPONDING</span>' : '<span class="bad">' + esc(data.status || 'WAITING') + '</span>';
    document.getElementById('inverter').textContent = data.inverterStatus || 'waiting';
    document.getElementById('inverter').className = 'value small ' + (data.ok && !data.stale && (data.inverterStatus || '').startsWith('available') ? 'ok' : 'warn');
    document.getElementById('pollstate').textContent = data.polling ? 'Reading: ' + (data.receivedCount || 0) + ' / 23 OIDs' : (data.stale ? 'STALE / INCOMPLETE VALUES' : data.status || 'Waiting');
    document.getElementById('target').textContent = (data.host || '-') + ':' + (data.snmpPort || 161);
    document.getElementById('response').textContent = data.responseMs == null ? '-' : data.responseMs + ' ms';
    document.getElementById('engine').textContent = data.engine || '-';
    document.getElementById('sysdescr').textContent = data.sysDescr || '-';
    document.getElementById('updated').textContent = data.lastDataAt || '-';
    document.getElementById('snmpwalk').textContent = data.snmpwalk ? 'found' : 'not found, built-in used';
    document.getElementById('count').textContent = (data.rows || []).length + ' / 23';
    document.getElementById('localips').textContent = (data.localIps || []).join(', ') || '-';
    document.getElementById('route').textContent = data.routeText || '-';
    document.getElementById('adapters').textContent = data.adapterText || '-';
    document.getElementById('ssid').textContent = data.wifiSsid || '-';
    if(data.host && document.activeElement !== document.getElementById('host')) document.getElementById('host').value = data.host;
    document.getElementById('error').innerHTML = data.error ? '<div class="error"><b>Error:</b> ' + esc(data.error) + (data.engineNote ? '<br><span class="small">' + esc(data.engineNote) + '</span>' : '') + '</div>' : '';
    document.getElementById('note').innerHTML = (!data.error && data.engineNote) ? '<div class="info">' + esc(data.engineNote) + '</div>' : '';
    const rows = data.rows || [];
    document.getElementById('rows').className = data.stale ? 'warn' : '';
    document.getElementById('rows').innerHTML = rows.length ? rows.map(r => `<tr><td>${r.leaf}</td><td>${esc(r.label)}</td><td class="${r.leaf===23?'full':''}">${esc(r.value)}</td><td class="${r.leaf===23?'full':''}">${esc(r.raw)}</td><td>${esc(r.type)}</td><td class="small">${esc(r.oid)}</td></tr>`).join('') : '<tr><td colspan="6">No inverter OIDs yet.</td></tr>';
  }catch(e){
    document.getElementById('error').innerHTML = '<div class="error"><b>Dashboard error:</b> ' + esc(e.message) + '</div>';
  } finally { refreshing = false; }
}
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>"""


class Handler(BaseHTTPRequestHandler):
    poller: Poller

    def log_message(self, fmt, *args):
        return

    def send_bytes(self, status: int, body: bytes, content_type: str):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/":
            body = HTML.encode("utf-8")
            self.send_bytes(200, body, "text/html; charset=utf-8")
            return
        if parsed.path == "/api/state":
            state = self.poller.snapshot()
            body = json.dumps(state, ensure_ascii=False).encode("utf-8")
            self.send_bytes(200, body, "application/json; charset=utf-8")
            return
        if parsed.path == "/api/set":
            qs = parse_qs(parsed.query)
            host = qs.get("host", [""])[0]
            community = qs.get("community", [DEFAULT_COMMUNITY])[0]
            try:
                self.poller.set_target(host, community)
            except ValueError as exc:
                self.send_bytes(400, json.dumps({"error": str(exc)}).encode("utf-8"), "application/json")
                return
            body = json.dumps({"ok": True, "host": host, "community": community}).encode("utf-8")
            self.send_bytes(200, body, "application/json; charset=utf-8")
            return
        self.send_bytes(404, b"Not found", "text/plain; charset=utf-8")

    def do_POST(self):
        parsed = urlparse(self.path)
        try:
            if parsed.path == "/api/fix-route":
                host = parse_qs(parsed.query).get("host", [""])[0]
                result = fix_ethernet_route(host)
                self.poller.set_target(host, self.poller.community)
                body = json.dumps(result, ensure_ascii=False).encode("utf-8")
                self.send_bytes(200, body, "application/json; charset=utf-8")
                return
            if parsed.path == "/api/restore-network":
                result = restore_pc_network()
                body = json.dumps(result, ensure_ascii=False).encode("utf-8")
                self.send_bytes(200, body, "application/json; charset=utf-8")
                return
        except Exception as exc:
            body = json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False).encode("utf-8")
            self.send_bytes(400, body, "application/json; charset=utf-8")
            return
        self.send_bytes(404, b"Not found", "text/plain; charset=utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=AUTO_HOST_TEXT)
    parser.add_argument("--community", default=DEFAULT_COMMUNITY)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--timeout", type=float, default=4.0)
    parser.add_argument("--retries", type=int, default=1)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent
    logs_dir = root / "logs"
    poller = Poller(args.host, args.community, args.timeout, args.retries, logs_dir)
    Handler.poller = poller
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    except OSError:
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=poller.run, daemon=True)
    diagnostics = threading.Thread(target=poller.network_loop, daemon=True)
    worker.start()
    diagnostics.start()
    url = f"http://127.0.0.1:{server.server_address[1]}/?host={args.host}"
    print("=" * 72)
    print("ETPL SNMP Test Dashboard")
    print("Version:", VERSION)
    print("Target:", args.host)
    print("Community:", args.community)
    print("Dashboard:", url)
    print("Logs:", logs_dir)
    print("snmpwalk.exe:", shutil.which("snmpwalk.exe") or shutil.which("snmpwalk") or "not found, built-in SNMP engine enabled")
    print("=" * 72)
    if not args.no_browser:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        poller.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
