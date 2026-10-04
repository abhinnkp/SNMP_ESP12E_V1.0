"""Package the verified R2 binary without overwriting the original baseline BIN."""
import csv
import hashlib
from pathlib import Path
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[1]
name = 'ETPL_SNMP_SITE_RECOVERY_20260930_R2.bin'
binary = ROOT / name
digest = hashlib.sha256(binary.read_bytes()).hexdigest().upper()
expected = '0A3044DA5196DF597B6D9E15413AB73E91D4437D49F88DDA75E3DA6B7F436F27'
if digest != expected:
    raise SystemExit('Candidate hash differs from the validated release; review before packaging.')
flash = (ROOT / 'FLASH_SNMP_20260930.bat').read_bytes()
flash = flash.replace(b'SNMP_NETWORK_SERVICE_20260930', b'SNMP_SITE_RECOVERY_20260930_R2')
flash = flash.replace(b'ETPL_SNMP_NETWORK_20260930.bin', name.encode())
flash = flash.replace(b'FLASH_SNMP_20260930.bat', b'FLASH_SNMP_SITE_RECOVERY_20260930_R2.bat')
flash = flash.replace(b'2BACDE07C55874441B62C2F60039A07DF1DBE0C7D556F84FC59A4296ADF88780',digest.encode())
(ROOT / 'FLASH_SNMP_SITE_RECOVERY_20260930_R2.bat').write_bytes(flash)

history = ROOT / 'results/baseline_package_manifest.csv'
if not history.exists():
    shutil.copy2(ROOT / 'PACKAGE_MANIFEST.csv', history)
with history.open(encoding='utf-8-sig',newline='') as handle:
    paths = {row['Path'] for row in csv.DictReader(handle) if (ROOT / row['Path']).is_file()}
paths.update([name,'FLASH_SNMP_SITE_RECOVERY_20260930_R2.bat','SITE_RECOVERY_R2.txt',
              'tools/build_site_candidate.py','tools/package_site_candidate.py',
              'tests/test_firmware_arp_native.py','tests/test_network_recovery_native.py',
              'tests/native/arp_regression.c','tests/native/packet_regression.c',
              'evidence/r2_regression_results.txt',
              'evidence/r2_flash_validation.json','evidence/r2_first_start.json'])
shutil.copy2(ROOT / 'results/r2_regression_results.txt',ROOT / 'evidence/r2_regression_results.txt')

notice = ('R2 CANDIDATE UPDATE - 30 SEPTEMBER 2026\n'
          'Current src/ and lib/ build SNMP_SITE_RECOVERY_20260930_R2.\n'
          'Read SITE_RECOVERY_R2.txt first; use FLASH_SNMP_SITE_RECOVERY_20260930_R2.bat for R2.\n'
          'R2 build, 52 automated checks and 12/12 bench network checks passed; site/soak testing is pending.\n'
          'Original BIN/flash launcher below remain unchanged for rollback.\n'
          'The following document describes the ORIGINAL baseline and its historical validation.\n\n')
for document in ['README_FIRST.txt','CODEX_HANDOFF.txt','VALIDATION.txt']:
    path = ROOT / document
    content = path.read_text(encoding='utf-8-sig')
    if not content.startswith('R2 CANDIDATE UPDATE'):
        path.write_text(notice+content,encoding='utf-8')
readme = ROOT / 'README.md'
content = readme.read_text(encoding='utf-8-sig')
if not content.startswith('> R2 candidate'):
    readme.write_text('> R2 candidate: current source builds `SNMP_SITE_RECOVERY_20260930_R2`. '
                     'Read [SITE_RECOVERY_R2.txt](SITE_RECOVERY_R2.txt) and use '
                     '`FLASH_SNMP_SITE_RECOVERY_20260930_R2.bat` for the new build. '
                     'Build, 52 automated checks and 12/12 bench network checks passed; site/soak testing is pending. '
                     'The original BIN and launcher below remain unchanged for rollback.\n\n'+content,encoding='utf-8')

manifest = ROOT / 'PACKAGE_MANIFEST.csv'
with manifest.open('w',encoding='utf-8',newline='') as handle:
    writer = csv.writer(handle,quoting=csv.QUOTE_ALL)
    writer.writerow(['Path','Bytes','SHA256'])
    for relative in sorted(paths):
        raw = (ROOT / relative).read_bytes()
        writer.writerow([relative,len(raw),hashlib.sha256(raw).hexdigest().upper()])
archive = ROOT / 'ETPL_SNMP_SITE_RECOVERY_20260930_R2.zip'
with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED) as package:
    for relative in sorted(paths | {'PACKAGE_MANIFEST.csv'}):
        package.write(ROOT / relative,relative)
print(f'Packaged {len(paths)+1} files: {archive.name}')
print(f'BIN SHA256: {digest}')
