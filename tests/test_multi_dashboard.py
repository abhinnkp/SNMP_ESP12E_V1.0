import csv
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import multi_snmp_dashboard as app


def sample(ok=True,mac='02E8265CDECC',uptime=100):
    return dict(observed_at=app.utc_now(),ping_ok=ok,ping_ms=1 if ok else None,
        snmp_online=ok,complete=ok,received_count=23 if ok else 0,
        rows=[{'leaf':1,'raw':mac,'value':mac,'oid':'1','label':'MAC','unit':''}] if ok else [],
        diagnostics={'device_uptime_seconds':uptime} if ok else None,
        error='' if ok else 'Timeout',http_error='',mac=mac if ok else '',
        inverter_status='Fresh' if ok else 'Unverified',data_fresh=ok)


class FleetTests(unittest.TestCase):
    def setUp(self):
        test_root=Path(__file__).resolve().parents[1]/'results'
        test_root.mkdir(exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=test_root)
        self.fleet=app.Fleet(self.temp.name,workers=2,interval=7)
    def tearDown(self):
        self.fleet.close();self.temp.cleanup()
    def add(self,host='192.168.2.1'):
        self.fleet.add([{'host':host}]);return next(k for k,c in self.fleet.configs.items() if c['host']==host)
    def test_range_and_csv(self):
        self.assertEqual(len(app.parse_targets('172.16.16.11-47')),37)
        self.assertEqual(app.parse_targets('name,host,community\nUPS,172.16.16.40,secret')[0]['name'],'UPS')
        with self.assertRaises(ValueError):app.parse_targets('172.16.16.47-11')
    def test_invalid_import_atomic(self):
        with self.assertRaises(ValueError):self.fleet.add([{'host':'192.168.2.1'},{'host':'invalid'}])
        self.assertEqual(self.fleet.snapshot()['counts']['total'],0)
    def test_duplicates_skipped(self):
        self.assertEqual(self.fleet.add([{'host':'192.168.2.1'}]*2),1)
    def test_saved_pause_and_secret_not_exported(self):
        self.fleet.add([{'host':'192.168.2.1','community':'private-secret'}]);key=next(iter(self.fleet.configs))
        self.fleet.pause(key,True)
        other=app.Fleet(self.temp.name)
        try:self.assertTrue(other.snapshot()['devices'][0]['paused'])
        finally:other.close()
        self.assertNotIn('private-secret',json.dumps(self.fleet.snapshot()))
        self.assertNotIn(b'private-secret',app.export_csv(self.fleet.snapshot()))
    def test_retained_values_stale_on_failure(self):
        key=self.add();self.fleet.apply_sample(key,sample());self.fleet.apply_sample(key,sample(False))
        d=self.fleet.snapshot()['devices'][0]
        self.assertEqual(d['rows'][0]['raw'],'02E8265CDECC');self.assertTrue(d['values_stale'])
        self.assertFalse(d['data_fresh']);self.assertEqual(d['failures'],1)
    def test_reset_detected(self):
        key=self.add();self.fleet.apply_sample(key,sample(uptime=100));self.fleet.apply_sample(key,sample(uptime=2))
        self.assertEqual(self.fleet.snapshot()['devices'][0]['resets_observed'],1)
    def test_duplicate_mac_across_hosts(self):
        for host in ['192.168.2.1','192.168.2.2']:self.fleet.apply_sample(self.add(host),sample())
        self.assertTrue(all(d['duplicate_mac'] for d in self.fleet.snapshot()['devices']))
    def test_formula_csv_escaped(self):
        key=self.add();self.fleet.states[key]['name']='=HYPERLINK("bad")'
        row=next(csv.DictReader(io.StringIO(app.export_csv(self.fleet.snapshot()).decode('utf-8-sig'))))
        self.assertTrue(row['name'].startswith("'="))
    def test_unexpected_worker_failure_recorded(self):
        key=self.add();self.fleet.sampler=lambda *a: (_ for _ in ()).throw(RuntimeError('bad response'))
        self.fleet.worker(key,self.fleet.configs[key]);d=self.fleet.snapshot()['devices'][0]
        self.assertEqual(d['failures'],1);self.assertFalse(d['snmp_online'])
    def test_offline_does_not_block_healthy_or_snapshot(self):
        blocked=threading.Event();release=threading.Event();healthy=threading.Event()
        def sampler(config,*args):
            if config['host']=='192.168.2.2':blocked.set();release.wait(3);return sample(False)
            healthy.set();return sample()
        self.fleet.sampler=sampler
        self.add('192.168.2.2');self.add('192.168.2.1');self.fleet.start()
        try:
            self.assertTrue(blocked.wait(1));self.assertTrue(healthy.wait(1))
            started=time.monotonic();self.fleet.snapshot();self.assertLess(time.monotonic()-started,.2)
        finally:release.set()
    def test_removed_inflight_not_restored(self):
        key=self.add();self.fleet.remove(key);self.fleet.apply_sample(key,sample())
        self.assertEqual(self.fleet.snapshot()['devices'],[])
    def test_pause_marks_retained_values(self):
        key=self.add();self.fleet.apply_sample(key,sample());self.fleet.pause(key,True)
        d=self.fleet.snapshot()['devices'][0];self.assertFalse(d['data_fresh']);self.assertTrue(d['values_stale'])


class IdentityTests(unittest.TestCase):
    def test_wrong_mac_never_exposes_other_device_values(self):
        poll={'rows':[{'leaf':1,'raw':'02E8265CDECC'}],'sysDescr':'ETPL'}
        class Response:
            def __enter__(self):return self
            def __exit__(self,*a):pass
            def read(self,*a):return b'{"mac":"02:E8:26:11:22:33","ok":true,"rs485_last_good_age_seconds":0}'
        with patch.object(app,'ping_device',return_value={'ping_ok':True,'ping_ms':1}),patch.object(app.ENGINE,'builtin_poll',return_value=poll),patch('urllib.request.OpenerDirector.open',return_value=Response()):
            result=app.read_sample(app.validate_device({'host':'192.168.2.1'}))
        self.assertTrue(result['identity_mismatch']);self.assertFalse(result['complete'])
        self.assertEqual(result['rows'],[]);self.assertIsNone(result['diagnostics']);self.assertFalse(result['data_fresh'])


if __name__=='__main__':unittest.main()
