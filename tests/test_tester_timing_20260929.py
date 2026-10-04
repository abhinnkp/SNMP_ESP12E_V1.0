import json
import socket
import subprocess
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[1]
BATCH = ROOT / 'TEST_SNMP_20260929.bat'
app = types.ModuleType('tester')
app.__file__ = str(BATCH)
source = BATCH.read_text(encoding='utf-8-sig').rsplit('###SNMP_DASHBOARD_PYTHON_START###', 1)[1].lstrip()
exec(compile(source, str(BATCH), 'exec'), app.__dict__)


class FakeAgent:
    """Loopback SNMP agent for delayed/lost responses, never contacts field devices."""
    def __init__(self, delay=0, drop_first=False, no_data=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.delay, self.drop_first, self.no_data = delay, drop_first, no_data
        self.requests = []
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        self.port_patch = patch.object(app, 'SNMP_PORT', self.port)
        self.port_patch.start()
        return self

    def __exit__(self, *args):
        self.stop_event.set()
        self.thread.join(5)
        self.sock.close()
        self.port_patch.stop()

    @staticmethod
    def decode_request(packet):
        _, outer, _ = app.read_tlv(packet, 0)
        _, _, pos = app.read_tlv(outer, 0)
        _, community, pos = app.read_tlv(outer, pos)
        tag, pdu, _ = app.read_tlv(outer, pos)
        if tag != 0xA0:
            raise AssertionError('Expected grouped GET')
        _, request_id, pos = app.read_tlv(pdu, 0)
        _, _, pos = app.read_tlv(pdu, pos)
        _, _, pos = app.read_tlv(pdu, pos)
        _, varbinds, _ = app.read_tlv(pdu, pos)
        pos, oids = 0, []
        while pos < len(varbinds):
            _, varbind, pos = app.read_tlv(varbinds, pos)
            _, raw, _ = app.read_tlv(varbind, 0)
            oids.append(app.oid_text(raw))
        return int.from_bytes(request_id, 'big'), community, oids

    def response(self, packet):
        request_id, community, oids = self.decode_request(packet)
        values = []
        for oid in oids:
            if oid == app.SYS_DESCR_OID:
                value = app.tlv(4, b'ESP8266 Local Monitoring Logger')
            else:
                leaf = app.leaf_from_oid(oid)
                if leaf == 1:
                    value = app.tlv(4, b'02E826123456')
                elif leaf == 23:
                    data = b'$NO_DATA#' if self.no_data else b'$02E826123456,230.0,50.0,0,0,230,50,52,0,0,0,0,0,0,0,0,25,0,0,0,0,0#'
                    value = app.tlv(4, data)
                else:
                    number = 230 if leaf in (2, 6) else 500
                    tag = 0x43 if leaf == 16 else 0x41 if leaf in (13, 14, 15) else 2 if leaf >= 18 else 0x42
                    value = app.tlv(tag, number.to_bytes(2, 'big'))
            values.append(app.tlv(0x30, app.tlv(6, app.oid_bytes(oid)) + value))
        pdu = app.integer(request_id) + app.integer(0) + app.integer(0) + app.tlv(0x30, b''.join(values))
        return app.tlv(0x30, app.integer(1) + app.tlv(4, community) + app.tlv(0xA2, pdu))

    def run(self):
        while not self.stop_event.is_set():
            try:
                packet, peer = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            self.requests.append(packet)
            if len(self.requests) == 1:
                if self.drop_first:
                    continue
                if self.stop_event.wait(self.delay):
                    return
            self.sock.sendto(self.response(packet), peer)


class TesterTimingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.poller = app.Poller('172.16.16.11', 'public', 4.0, 1, Path(self.temp.name))

    def test_delayed_reply_over_old_timeout_returns_all_23_oids(self):
        with FakeAgent(delay=2.4) as agent:
            start = time.monotonic()
            result = app.builtin_poll('127.0.0.1', 'public', 4.0, 1)
            duration = time.monotonic() - start
            self.assertEqual(len(result['rows']), 23)
            self.assertEqual(len(agent.requests), 4)
            self.assertGreater(duration, 2.0)
            self.assertLess(duration, 4.0)
            self.assertTrue(all(len(packet) < 512 for packet in agent.requests))

    def test_lost_first_packet_retries_same_target(self):
        with FakeAgent(drop_first=True) as agent:
            result = app.builtin_poll('127.0.0.1', 'public', 0.1, 1)
            self.assertEqual(len(result['rows']), 23)
            self.assertEqual(len(agent.requests), 5)

    def test_exact_ip_timeout_does_not_scan_or_try_second_engine(self):
        with patch.object(app, 'net_snmp_poll', side_effect=TimeoutError('timed out')), \
             patch.object(app, 'builtin_poll') as fallback, \
             patch.object(app, 'auto_discover_host') as scan:
            self.poller.poll_once()
        scan.assert_not_called()
        fallback.assert_not_called()
        state = self.poller.snapshot()
        self.assertEqual(state['host'], '172.16.16.11')
        self.assertFalse(state['snmpOnline'])
        self.assertFalse(state['ok'])

    def test_missing_executable_uses_builtin_and_no_data_is_separate(self):
        with FakeAgent(no_data=True), patch.object(app, 'net_snmp_poll', side_effect=app.ToolUnavailable('not found')):
            self.poller.set_target('127.0.0.1', 'public')
            self.poller.poll_once()
        state = self.poller.snapshot()
        self.assertTrue(state['snmpOnline'])
        self.assertEqual(state['inverterStatus'], 'NO_DATA')
        self.assertEqual(state['status'], 'inverter NO_DATA')

    def test_failed_refresh_retains_rows_as_stale(self):
        with FakeAgent(), patch.object(app, 'net_snmp_poll', side_effect=app.ToolUnavailable()):
            self.poller.set_target('127.0.0.1', 'public')
            self.poller.poll_once()
        before = self.poller.snapshot()
        with patch.object(app, 'net_snmp_poll', side_effect=TimeoutError('timed out')):
            self.poller.poll_once()
        after = self.poller.snapshot()
        self.assertEqual(after['rows'], before['rows'])
        self.assertEqual(after['lastDataAt'], before['lastDataAt'])
        self.assertTrue(after['stale'])
        self.assertFalse(after['ok'])
        self.poller.set_target('172.16.16.12', 'public')
        self.assertEqual(self.poller.snapshot()['rows'], [])

    def test_changed_target_cannot_be_overwritten_by_old_poll(self):
        entered = threading.Event()
        release = threading.Event()

        def old_poll(*args):
            entered.set()
            release.wait(2)
            return {'rows': [], 'sysDescr': 'old device', 'engine': 'fake'}

        with patch.object(app, 'net_snmp_poll', side_effect=old_poll):
            worker = threading.Thread(target=self.poller.poll_once)
            worker.start()
            self.assertTrue(entered.wait(1))
            self.poller.set_target('172.16.16.12', 'public')
            release.set()
            worker.join(2)
        state = self.poller.snapshot()
        self.assertEqual(state['host'], '172.16.16.12')
        self.assertEqual(state['rows'], [])
        self.assertFalse(state['ok'])

    def test_net_snmp_uses_4_grouped_gets_and_4_second_timeout(self):
        def run(command, **kwargs):
            self.assertIn('-t', command)
            self.assertEqual(command[command.index('-t') + 1], '4.0')
            self.assertTrue(command[0].endswith('snmpget.exe'))
            oids = command[command.index('-On') + 2:]
            lines = []
            for oid in oids:
                leaf = app.leaf_from_oid(oid)
                value = 'STRING: "ESP8266 Local Monitoring Logger"' if oid == app.SYS_DESCR_OID else 'STRING: "$NO_DATA#"' if leaf == 23 else 'STRING: "02E826123456"' if leaf == 1 else 'Timeticks: (500) 0:00:05.00' if leaf == 16 else 'Gauge32: 500'
                lines.append(f'.{oid} = {value}')
            return subprocess.CompletedProcess(command, 0, '\n'.join(lines), '')

        with patch.object(app.shutil, 'which', return_value='snmpget.exe'), patch.object(app.subprocess, 'run', side_effect=run) as invoke:
            result = app.net_snmp_poll('172.16.16.11', 'public', 4.0, 1)
        self.assertEqual(invoke.call_count, 4)
        self.assertEqual(len(result['rows']), 23)
        self.assertEqual(next(row['raw'] for row in result['rows'] if row['leaf'] == 16), 500)

    def test_incomplete_response_is_not_success(self):
        with patch.object(app, 'snmp_request', return_value=[(app.SYS_DESCR_OID, 4, b'agent')]):
            with self.assertRaisesRegex(RuntimeError, 'do not match'):
                app.builtin_poll('172.16.16.11', 'public', 4, 1)

    def test_poll_deadline_is_enforced(self):
        with patch.object(app.time, 'monotonic', return_value=100):
            with self.assertRaises(TimeoutError):
                app.request_timeout(99, 4, 1)
            self.assertEqual(app.request_timeout(102, 4, 1), 1)

    def test_state_api_never_waits_for_network_diagnostics(self):
        handler = type('TestHandler', (app.Handler,), {'poller': self.poller})
        server = app.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with patch.object(app, 'network_context', side_effect=AssertionError('diagnostics in UI path')):
                start = time.monotonic()
                with urlopen(f'http://127.0.0.1:{server.server_port}/api/state', timeout=1) as response:
                    state = json.load(response)
                self.assertLess(time.monotonic() - start, 0.5)
                self.assertEqual(state['host'], '172.16.16.11')
        finally:
            server.shutdown()
            server.server_close()
            worker.join(2)

    def test_discovery_honors_site_23_subnet(self):
        with patch.object(app, 'local_ipv4_addresses', return_value=['172.16.17.100']), \
             patch.object(app, 'windows_network_details', return_value=([{'ip': '172.16.17.100', 'prefix': 23, 'status': 'Up'}], '')):
            self.assertIn('172.16.16.11', app.discovery_candidates())

    def test_log_write_failure_does_not_stop_polling(self):
        with patch.object(Path, 'open', side_effect=OSError('disk full')):
            self.poller.write_log({'ok': True})
        self.assertIn('disk full', self.poller.snapshot()['logWarning'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
