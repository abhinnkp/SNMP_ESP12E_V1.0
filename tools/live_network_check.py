"""Read-only checks of one explicitly selected field device, without discovery."""
import argparse
import ipaddress
import json
import re
import subprocess
import time
import types
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BATCH = ROOT / "TEST_SNMP_20260929.bat"


def load_tester():
    app = types.ModuleType("field_tester")
    app.__file__ = str(BATCH)
    source = BATCH.read_text(encoding="utf-8-sig").rsplit("###SNMP_DASHBOARD_PYTHON_START###", 1)[1].lstrip()
    exec(compile(source, str(BATCH), "exec"), app.__dict__)
    return app


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--expected-mac", help="Optional expected device MAC, with or without separators")
    parser.add_argument("--community", default="public")
    parser.add_argument("--count", type=int, default=12)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        host = str(ipaddress.IPv4Address(args.host))
    except ipaddress.AddressValueError as exc:
        parser.error(str(exc))
    if args.count < 1:
        parser.error("--count must be at least 1")
    expected_mac = None
    if args.expected_mac:
        expected_mac = args.expected_mac.replace(":", "").replace("-", "").upper()
        if not re.fullmatch(r"[0-9A-F]{12}", expected_mac):
            parser.error("--expected-mac must contain exactly 12 hexadecimal digits")
    app = load_tester()
    result = {"host": host, "started_utc": datetime.now(timezone.utc).isoformat(),
              "expected_mac": expected_mac, "read_only": True, "samples": []}
    for index in range(args.count):
        start = time.monotonic()
        sample = {"index": index + 1, "utc": datetime.now(timezone.utc).isoformat()}
        try:
            ping = subprocess.run(["ping.exe", "-n", "1", "-w", "1000", host],
                                  capture_output=True, text=True, timeout=5, errors="replace")
            sample["ping"] = ping.stdout
            # Count actual target replies, not Windows' received-unreachable count.
            sample["ping_ok"] = bool(re.search(
                rf"(?<![\d.]){re.escape(host)}(?![\d.])[^\r\n]*TTL=\d+", ping.stdout, re.I))
        except (OSError, subprocess.TimeoutExpired) as exc:
            sample["ping_ok"] = False
            sample["ping_error"] = str(exc)
        snmp_start = time.monotonic()
        try:
            required_oids = [app.SYS_DESCR_OID, app.BASE_OID + ".1", app.BASE_OID + ".23"]
            values = app.snmp_request(host, args.community, required_oids, 0xA0, 2.0, 0)
            decoded = {oid: app.format_snmp_value(tag, raw)[1] for oid, tag, raw in values}
            sample["snmp_values"] = decoded
            mac = str(decoded.get(app.BASE_OID + ".1", "")).replace(":", "").replace("-", "").upper()
            sample["identity_matches"] = mac == expected_mac if expected_mac else None
            sample["snmp_ok"] = (set(decoded) == set(required_oids)
                                 and bool(re.fullmatch(r"[0-9A-F]{12}", mac))
                                 and sample["identity_matches"] is not False)
            if sample["snmp_ok"]:
                sample["full_poll"] = app.builtin_poll(host, args.community, 4.0, 1)
        except Exception as exc:
            sample["snmp_ok"] = False
            sample["snmp_error"] = str(exc)
        sample["snmp_elapsed_ms"] = round((time.monotonic() - snmp_start) * 1000)
        # HTTP must remain independent: a failed UDP listener may still expose
        # the uptime, ARP-conflict and Ethernet fault evidence we need.
        try:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open(f"http://{host}/api/inverter", timeout=3) as response:
                sample["diagnostics"] = json.loads(response.read(8192))
        except Exception as exc:
            sample["http_error"] = str(exc)
        result["samples"].append(sample)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"{index + 1}/{args.count} ping={sample['ping_ok']} "
              f"snmp={sample['snmp_ok']} {sample.get('snmp_error', '')}", flush=True)
        if index + 1 < args.count:
            time.sleep(max(0, 5.0 - (time.monotonic() - start)))
    result["finished_utc"] = datetime.now(timezone.utc).isoformat()
    result["ping_successes"] = sum(s["ping_ok"] for s in result["samples"])
    result["snmp_successes"] = sum(s["snmp_ok"] for s in result["samples"])
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"Result: ping {result['ping_successes']}/{args.count}, "
          f"SNMP {result['snmp_successes']}/{args.count}; saved {args.output}")


if __name__ == "__main__":
    main()
