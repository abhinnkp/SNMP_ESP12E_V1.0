"""Compile and run the vendored C ARP implementation, not a Python imitation."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
UTILITY = ROOT / 'lib/EthernetENC/src/utility'

class FirmwareArpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        bundled = ROOT / 'tools/native_compiler/tcc/tcc.exe'
        compiler = os.environ.get('NATIVE_CC') or (str(bundled) if bundled.exists() else shutil.which('gcc'))
        if not compiler:
            raise unittest.SkipTest('Install a native C compiler or set NATIVE_CC')
        output = ROOT / 'results/native'
        output.mkdir(parents=True, exist_ok=True)
        cls.exe = output / 'arp_regression.exe'
        subprocess.run([compiler, '-D__BYTE_ORDER__=1234', '-I'+str(UTILITY),
                        str(ROOT / 'tests/native/arp_regression.c'), '-o', str(cls.exe)], check=True)
        cls.packet_exe = output / 'packet_regression.exe'
        subprocess.run([compiler, '-D__BYTE_ORDER__=1234', '-I'+str(UTILITY),
                        str(ROOT / 'tests/native/packet_regression.c'), '-o', str(cls.packet_exe)], check=True)

def add_case(name):
    def test(self):
        subprocess.run([str(self.exe), name], check=True, capture_output=True, text=True, timeout=5)
    setattr(FirmwareArpTests, 'test_'+name, test)

for case in ['wrap_expiry', 'wrap_eviction', 'halfword_zero', 'gateway_announcement',
             'gateway_reply_announcement', 'invalid_arp', 'short_ip', 'init_clock',
             'duplicate_ip', 'site_23', 'routed_reply', 'announcement_format', 'gateway_request']:
    add_case(case)

def add_packet_case(name):
    def test(self):
        subprocess.run([str(self.packet_exe),name],check=True,capture_output=True,text=True,timeout=5)
    setattr(FirmwareArpTests,'test_packet_'+name,test)

for case in ['short_udp','bad_udp_length','udp_length_underflow','short_tcp',
             'bad_tcp_offset','short_icmp','valid_udp','valid_icmp']:
    add_packet_case(case)

if __name__ == '__main__':
    unittest.main(verbosity=2)
