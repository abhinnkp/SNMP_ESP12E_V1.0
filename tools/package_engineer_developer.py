"""Create explicit field/developer kits without live settings or local caches."""
import csv
import hashlib
import io
import json
from pathlib import Path
import shutil
import urllib.request
import zipfile

ROOT=Path(__file__).resolve().parents[1]
BIN='ETPL_SNMP_SITE_RECOVERY_20260930_R2.bin'
EXPECTED='0A3044DA5196DF597B6D9E15413AB73E91D4437D49F88DDA75E3DA6B7F436F27'
if hashlib.sha256((ROOT/BIN).read_bytes()).hexdigest().upper()!=EXPECTED:
    raise SystemExit('R2 BIN does not match validated firmware. Validate the new release first.')
shutil.copy2(ROOT/'results/multi_release_regression.txt',ROOT/'evidence/multi_release_regression.txt')
server=ROOT/'_snmp_multi_runtime/server.json'
if server.exists():
    url=json.loads(server.read_text())['url']
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url+'api/state',timeout=3) as response:
        live=json.load(response)
    (ROOT/'evidence/multi_live_validation.json').write_text(json.dumps(live,indent=2),encoding='utf-8')

field={BIN,'FLASH_SNMP_SITE_RECOVERY_20260930_R2.bat','TEST_SNMP_MULTI_20260930.bat',
    'TEST_SNMP_20260929.bat','tools/multi_snmp_dashboard.py','tools/multi_snmp_dashboard.html',
    'tools/live_network_check.py','README_MULTI_DEVICE.txt','FIELD_ENGINEER_START_HERE.txt',
    'SITE_RECOVERY_R2.txt','DEVICE_LIST_EXAMPLE.csv'}
developer=field|{'DEVELOPER_START_HERE.txt','platformio.ini','README.md','README_FIRST.txt',
    'CODEX_HANDOFF.txt','VALIDATION.txt','.gitignore','.gitattributes',
    'ETPL_SNMP_NETWORK_20260930.bin','FLASH_SNMP_20260930.bat'}
for folder in ['src','lib','tests','tools']:
    for path in (ROOT/folder).rglob('*'):
        if path.is_file() and '__pycache__' not in path.parts and path.suffix not in {'.pyc','.pyo','.zip'}:
            developer.add(path.relative_to(ROOT).as_posix())
developer.update({'evidence/r2_regression_results.txt','evidence/r2_flash_validation.json',
    'evidence/r2_first_start.json','evidence/multi_release_regression.txt','evidence/multi_live_validation.json'})

def package(name,paths):
    manifest=io.StringIO(newline='')
    writer=csv.writer(manifest)
    writer.writerow(['Path','Bytes','SHA256'])
    for relative in sorted(paths):
        raw=(ROOT/relative).read_bytes()
        writer.writerow([relative,len(raw),hashlib.sha256(raw).hexdigest().upper()])
    target=ROOT/name
    with zipfile.ZipFile(target,'w',compression=zipfile.ZIP_DEFLATED) as archive:
        for relative in sorted(paths):archive.write(ROOT/relative,relative)
        archive.writestr('PACKAGE_MANIFEST.csv',manifest.getvalue().encode('utf-8'))
    with zipfile.ZipFile(target) as archive:
        if archive.testzip():raise RuntimeError('ZIP CRC failed: '+name)
        for row in csv.DictReader(io.StringIO(archive.read('PACKAGE_MANIFEST.csv').decode('utf-8'))):
            raw=archive.read(row['Path'])
            if len(raw)!=int(row['Bytes']) or hashlib.sha256(raw).hexdigest().upper()!=row['SHA256']:
                raise RuntimeError('Manifest verification failed: '+row['Path'])
    print(f'{name}: {len(paths)+1} files, {target.stat().st_size:,} bytes; CRC and SHA256 manifest verified.')

package('ETPL_FIELD_ENGINEER_20260930_R2.zip',field)
package('ETPL_DEVELOPER_20260930_R2.zip',developer)
