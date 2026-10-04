import contextlib
import importlib.util
import io
import json
import subprocess
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('live_check', ROOT / 'tools/live_network_check.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class LiveCheckTests(unittest.TestCase):
    def run_check(self, extra=(), ping=None, snmp_error=None):
        base = '1.3.6.1.4.1.12345.1.23'
        descr = '1.3.6.1.2.1.1.1.0'
        app = types.SimpleNamespace(
            BASE_OID=base, SYS_DESCR_OID=descr,
            snmp_request=Mock(return_value=[
                (descr, 4, 'ESP8266'), (base + '.1', 4, '02E8265CDECC'),
                (base + '.23', 4, '$NO_DATA#')]),
            format_snmp_value=lambda tag, raw: ('STRING', raw),
            builtin_poll=Mock(return_value={'rows': [], 'packet': '$NO_DATA#'}))
        if snmp_error:
            app.snmp_request.side_effect = snmp_error
        default_ping = subprocess.CompletedProcess([], 0,
            'Reply from 192.168.2.1: bytes=32 time=1ms TTL=128', '')
        ping_runner = Mock(side_effect=ping) if isinstance(ping, Exception) else Mock(return_value=ping or default_ping)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'result.json'
            argv = ['live_network_check.py', '--host', '192.168.2.1', '--count', '1',
                    '--output', str(output), *extra]
            with patch.object(check, 'load_tester', return_value=app), \
                 patch.object(check.subprocess, 'run', ping_runner), \
                 patch.object(check.urllib.request, 'build_opener', side_effect=OSError('mock HTTP offline')), \
                 patch('sys.argv', argv), contextlib.redirect_stdout(io.StringIO()):
                check.main()
            return json.loads(output.read_text(encoding='utf-8')), app

    def test_embedded_tester_loads_from_package_root(self):
        app = check.load_tester()
        self.assertEqual(app.BASE_OID, '1.3.6.1.4.1.12345.1.23')
        self.assertTrue(callable(app.snmp_request))

    def test_expected_mac_and_no_data_are_separate(self):
        result, _ = self.run_check(['--expected-mac', '02:e8:26:5c:de:cc'])
        self.assertEqual(result['snmp_successes'], 1)
        self.assertEqual(result['ping_successes'], 1)
        self.assertIn('$NO_DATA#', result['samples'][0]['snmp_values'].values())

    def test_no_expected_mac_does_not_assume_bench_identity(self):
        result, app = self.run_check(['--community', 'test-community'])
        self.assertEqual(result['snmp_successes'], 1)
        self.assertIsNone(result['samples'][0]['identity_matches'])
        self.assertEqual(app.snmp_request.call_args.args[1], 'test-community')

    def test_mismatched_mac_is_not_accepted(self):
        result, app = self.run_check(['--expected-mac', '001122334455'])
        self.assertEqual(result['snmp_successes'], 0)
        app.builtin_poll.assert_not_called()

    def test_local_host_unreachable_is_not_ping_success(self):
        ping = subprocess.CompletedProcess([], 0,
            'Reply from 192.168.2.106: Destination host unreachable. Received = 1', '')
        result, _ = self.run_check(ping=ping)
        self.assertEqual(result['ping_successes'], 0)
        self.assertEqual(result['snmp_successes'], 1)

    def test_ping_command_timeout_does_not_skip_snmp(self):
        result, _ = self.run_check(ping=subprocess.TimeoutExpired('ping.exe', 5))
        self.assertEqual(result['ping_successes'], 0)
        self.assertEqual(result['snmp_successes'], 1)
        self.assertIn('ping_error', result['samples'][0])

    def test_failed_snmp_still_attempts_http_diagnostics(self):
        result, _ = self.run_check(snmp_error=TimeoutError('UDP timed out'))
        sample = result['samples'][0]
        self.assertFalse(sample['snmp_ok'])
        self.assertEqual(sample['http_error'], 'mock HTTP offline')

    def test_invalid_count_is_rejected_without_network_access(self):
        with patch('sys.argv', ['check', '--host', '192.168.2.1', '--count', '0',
                                '--output', 'unused.json']), \
             patch.object(check, 'load_tester') as load, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                check.main()
        self.assertEqual(error.exception.code, 2)
        load.assert_not_called()


if __name__ == '__main__':
    unittest.main(verbosity=2)
