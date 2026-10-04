"""Build using Python's CA bundle plus the Windows trusted certificate stores.

This keeps TLS verification enabled when a managed network uses a trusted proxy.
No system trust or PlatformIO settings are changed.
"""
import os
from pathlib import Path
import ssl
import subprocess
import sys
import certifi

ROOT = Path(__file__).resolve().parents[1]
output = ROOT / 'results/build'
output.mkdir(parents=True, exist_ok=True)
bundle = output / 'windows_trusted_ca.pem'
certs = [Path(certifi.where()).read_text(encoding='ascii')]
if sys.platform == 'win32':
    for store in ('ROOT', 'CA'):
        for cert, encoding, trust in ssl.enum_certificates(store):
            if encoding == 'x509_asn' and (trust is True or ssl.Purpose.SERVER_AUTH.oid in trust):
                certs.append(ssl.DER_cert_to_PEM_cert(cert))
bundle.write_text('\n'.join(certs), encoding='ascii')
env = os.environ.copy()
env['REQUESTS_CA_BUNDLE'] = str(bundle)
env['SSL_CERT_FILE'] = str(bundle)
env['PLATFORMIO_SETTING_ENABLE_TELEMETRY'] = 'no'
sys.exit(subprocess.call([sys.executable, '-m', 'platformio', 'run',
                          '-e', 'esp12e_nodemcu_115200'], cwd=ROOT, env=env))
