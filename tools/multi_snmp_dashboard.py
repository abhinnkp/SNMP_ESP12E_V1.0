"""Local multi-controller field tester using the existing verified SNMP engine."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import csv
from datetime import datetime, timezone
import io
import ipaddress
import json
import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse
import urllib.request
import uuid
import webbrowser

from live_network_check import load_tester

ROOT = Path(__file__).resolve().parents[1]
ENGINE = load_tester()
VERSION = 'SNMP_MULTI_20260930'
MAX_DEVICES = 128


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def normalize_mac(value):
    return str(value or '').replace(':', '').replace('-', '').upper()


def validate_device(value):
    host = str(ipaddress.IPv4Address(str(value.get('host', '')).strip()))
    address = ipaddress.IPv4Address(host)
    if address.is_multicast or address.is_unspecified or host == '255.255.255.255':
        raise ValueError('Enter a unicast device IPv4 address.')
    name = str(value.get('name') or host).strip()
    community = str(value.get('community') or 'public').strip()
    mac = normalize_mac(value.get('expected_mac'))
    if not name or len(name) > 100 or any(ord(c) < 32 for c in name):
        raise ValueError('Device name must be 1–100 printable characters.')
    if not community or len(community.encode('utf-8')) > 128:
        raise ValueError('Community must be 1–128 bytes.')
    if mac and not re.fullmatch(r'[0-9A-F]{12}', mac):
        raise ValueError('Expected MAC must have 12 hexadecimal digits.')
    return {'host': host, 'name': name, 'community': community, 'expected_mac': mac}


def parse_targets(text):
    text = str(text).strip()
    if not text:
        raise ValueError('Enter IP addresses or select a CSV file.')
    first = next(csv.reader([text.splitlines()[0]]))
    if 'host' in [cell.strip().lower() for cell in first]:
        reader = csv.DictReader(io.StringIO(text))
        reader.fieldnames = [cell.strip().lower() for cell in reader.fieldnames]
        result = [validate_device(row) for row in reader]
    else:
        result = []
        for token in re.split(r'[\s,;]+', text):
            match = re.fullmatch(r'(\d+\.\d+\.\d+\.)(\d+)-(\d+)', token)
            if match:
                start, end = int(match[2]), int(match[3])
                if not 0 <= start <= end <= 255:
                    raise ValueError('Use an ascending last-octet range, e.g. 172.16.16.11-47.')
                result.extend(validate_device({'host': match[1]+str(n)}) for n in range(start,end+1))
            else:
                result.append(validate_device({'host': token}))
            if len(result) > MAX_DEVICES:
                raise ValueError(f'Maximum {MAX_DEVICES} devices.')
    if not result or len(result) > MAX_DEVICES:
        raise ValueError(f'Import must contain 1–{MAX_DEVICES} devices.')
    return result


def ping_device(host):
    try:
        result = subprocess.run(['ping.exe','-n','1','-w','1000',host],capture_output=True,
                                text=True,timeout=4,errors='replace')
        ok = bool(re.search(rf'(?<![\d.]){re.escape(host)}(?![\d.])[^\r\n]*TTL=\d+',result.stdout,re.I))
        match = re.search(r'time([=<])(\d+)\s*ms',result.stdout,re.I)
        return {'ping_ok': ok, 'ping_ms': int(match[2]) if ok and match else None,
                'ping_error': '' if ok else result.stdout.strip()[-800:]}
    except (OSError,subprocess.TimeoutExpired) as exc:
        return {'ping_ok': False,'ping_ms': None,'ping_error': str(exc)}


def read_sample(config, timeout=4.0, retries=1):
    started = time.monotonic()
    result = {'observed_at':utc_now(), **ping_device(config['host']), 'snmp_online':False,
              'complete':False,'received_count':0,'rows':[],'error':'','diagnostics':None,
              'http_error':'','mac':'','inverter_status':'Unverified','data_fresh':False}
    partial = {}
    snmp_start = time.monotonic()
    try:
        poll = ENGINE.builtin_poll(config['host'],config['community'],timeout,retries,
                                   progress=lambda progress: partial.update(progress))
        result.update(rows=poll['rows'],sys_descr=poll['sysDescr'],complete=True)
    except Exception as exc:
        result['error'] = str(exc) or type(exc).__name__
        result['rows'] = partial.get('rows',[])
        result['sys_descr'] = partial.get('sysDescr','')
    result['snmp_ms'] = round((time.monotonic()-snmp_start)*1000)
    result['received_count'] = len(result['rows'])
    result['snmp_online'] = bool(result['rows'])
    result['mac'] = normalize_mac(next((row['raw'] for row in result['rows'] if row['leaf']==1),''))
    try:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(f"http://{config['host']}/api/inverter",timeout=2) as response:
            diagnostics = json.loads(response.read(16384))
        if not isinstance(diagnostics,dict):
            raise ValueError('Diagnostics response is not a JSON object.')
        result['diagnostics'] = diagnostics
    except Exception as exc:
        result['http_error'] = str(exc) or type(exc).__name__

    expected = config['expected_mac']
    diagnostics = result['diagnostics'] or {}
    http_mac = normalize_mac(diagnostics.get('mac'))
    wrong_identity = (bool(result['mac']) and not re.fullmatch(r'[0-9A-F]{12}',result['mac']))
    wrong_identity |= bool(expected and (result['mac'] and result['mac']!=expected or http_mac and http_mac!=expected))
    wrong_identity |= bool(result['mac'] and http_mac and result['mac']!=http_mac)
    if result['complete'] and not result['mac']:
        wrong_identity = True
    result['identity_mismatch'] = wrong_identity
    if wrong_identity:
        result.update(complete=False,error='MAC identity mismatch — verify the target or duplicate IP.',
                      inverter_status='Identity mismatch')
        # Values from the wrong device must never replace trusted retained values.
        result['rows'] = []
        result['diagnostics'] = None
    else:
        packet = next((str(row['raw']) for row in result['rows'] if row['leaf']==23),'')
        age = diagnostics.get('rs485_last_good_age_seconds')
        if diagnostics:
            if diagnostics.get('ok') is False or diagnostics.get('packet')=='$NO_DATA#':
                result['inverter_status'] = 'NO_DATA'
            elif diagnostics.get('ok') is True and isinstance(age,(int,float)):
                result['data_fresh'] = 0<=age<=10 and diagnostics.get('rs485_consecutive_failures',0)==0
                result['inverter_status'] = 'Fresh' if result['data_fresh'] else 'Cached'
        elif packet=='$NO_DATA#':
            result['inverter_status'] = 'NO_DATA'
        elif packet.startswith('$') and packet.endswith('#') and ',' in packet:
            result['inverter_status'] = 'Cache — age unverified'
    result['elapsed_ms'] = round((time.monotonic()-started)*1000)
    return result


def empty_state(config):
    return {'id':config['id'],'name':config['name'],'host':config['host'],
            'expected_mac':config['expected_mac'],'polling':False,'paused':False,
            'snmp_online':False,'complete':False,'ping_ok':None,'ping_ms':None,
            'rows':[],'values_stale':False,'diagnostics':None,'diagnostics_stale':False,
            'inverter_status':'Waiting','data_fresh':False,'error':'','http_error':'',
            'mac':'','polls':0,'successes':0,'failures':0,'consecutive_failures':0,
            'resets_observed':0,'history':[],'last_ok_at':None,'last_ok_epoch':None}


class Fleet:
    def __init__(self,runtime,workers=8,interval=7.0,timeout=4.0,retries=1,sampler=read_sample):
        self.runtime = Path(runtime)
        self.runtime.mkdir(parents=True,exist_ok=True)
        self.path = self.runtime/'devices.json'
        self.lock = threading.RLock()
        self.stop_event = threading.Event()
        self.wake = threading.Event()
        self.workers,self.interval,self.timeout,self.retries = workers,interval,timeout,retries
        self.sampler = sampler
        self.configs,self.states,self.due,self.active = {},{},{},set()
        self.log_warning = ''
        self.pool = ThreadPoolExecutor(max_workers=workers,thread_name_prefix='snmp-device')
        self.logger = logging.getLogger('fleet-'+uuid.uuid4().hex)
        self.logger.setLevel(logging.INFO)
        self.logger.propagate = False
        handler = RotatingFileHandler(self.runtime/'samples.jsonl',maxBytes=10_000_000,backupCount=3,encoding='utf-8')
        handler.setFormatter(logging.Formatter('%(message)s'))
        self.logger.addHandler(handler)
        if self.path.exists():
            configs = json.loads(self.path.read_text(encoding='utf-8'))
            if not isinstance(configs,list) or len(configs)>MAX_DEVICES:
                raise ValueError('Saved device list is invalid; check '+str(self.path))
            seen = set()
            for original in configs:
                config = {**validate_device(original),'id':str(original.get('id') or uuid.uuid4().hex),
                          'paused':bool(original.get('paused',False))}
                if config['host'] in seen or config['id'] in self.configs:
                    raise ValueError('Saved device list contains duplicate targets.')
                seen.add(config['host'])
                self.configs[config['id']] = config
                self.states[config['id']] = empty_state(config)
                self.states[config['id']]['paused'] = config['paused']
                self.due[config['id']] = 0.0

    def save_locked(self):
        temporary = self.path.with_suffix('.tmp')
        temporary.write_text(json.dumps(list(self.configs.values()),indent=2),encoding='utf-8')
        temporary.replace(self.path)

    def add(self,items):
        configs = [validate_device(item) for item in items]
        with self.lock:
            seen = {config['host'] for config in self.configs.values()}
            additions = []
            for config in configs:
                if config['host'] not in seen:
                    seen.add(config['host'])
                    additions.append({**config,'id':uuid.uuid4().hex,'paused':False})
            if len(self.configs)+len(additions)>MAX_DEVICES:
                raise ValueError(f'Maximum {MAX_DEVICES} devices.')
            previous = self.configs.copy()
            self.configs.update({config['id']:config for config in additions})
            try: self.save_locked()
            except Exception:
                self.configs=previous
                raise
            for config in additions:
                self.states[config['id']]=empty_state(config)
                self.due[config['id']]=0.0
        self.wake.set()
        return len(additions)

    def remove(self,device_id):
        with self.lock:
            if device_id not in self.configs: raise ValueError('Device not found.')
            previous=self.configs.copy()
            del self.configs[device_id]
            try: self.save_locked()
            except Exception:
                self.configs=previous
                raise
            self.states.pop(device_id,None)
            self.due.pop(device_id,None)
        self.wake.set()

    def pause(self,device_id,paused):
        with self.lock:
            if device_id not in self.configs: raise ValueError('Device not found.')
            old=self.configs[device_id]['paused']
            self.configs[device_id]['paused']=paused
            try: self.save_locked()
            except Exception:
                self.configs[device_id]['paused']=old
                raise
            self.states[device_id]['paused']=paused
            if not paused: self.due[device_id]=0.0
        self.wake.set()

    def poll_now(self,device_id=None):
        with self.lock:
            if device_id and device_id not in self.configs: raise ValueError('Device not found.')
            for key in ([device_id] if device_id else list(self.configs)):
                self.due[key]=0.0
        self.wake.set()

    def apply_sample(self,device_id,sample):
        with self.lock:
            if device_id not in self.states: return
            state=self.states[device_id]
            old_rows=state['rows']
            old_diagnostics=state['diagnostics']
            history=state['history']
            previous_uptime=(old_diagnostics or {}).get('device_uptime_seconds')
            current_uptime=(sample.get('diagnostics') or {}).get('device_uptime_seconds')
            if isinstance(previous_uptime,(int,float)) and isinstance(current_uptime,(int,float)) and current_uptime<previous_uptime:
                state['resets_observed']+=1
            complete=sample['complete']
            state.update(sample)
            state['polls']+=1
            state['successes' if complete else 'failures']+=1
            state['consecutive_failures']=0 if complete else state['consecutive_failures']+1
            state['rows']=sample['rows'] if complete else old_rows or sample['rows']
            state['values_stale']=bool(state['rows']) and not complete
            state['diagnostics']=sample.get('diagnostics') or old_diagnostics
            state['diagnostics_stale']=not bool(sample.get('diagnostics')) and bool(old_diagnostics)
            if complete:
                state['last_ok_at']=sample['observed_at']
                state['last_ok_epoch']=time.time()
            if not complete and sample['inverter_status']=='Unverified' and state['rows']:
                state['inverter_status']='Stale'
            history.append({'at':sample['observed_at'],'ok':complete,'ping':sample['ping_ok'],
                            'ms':sample.get('snmp_ms'),'data':sample['inverter_status']})
            state['history']=history[-80:]
            state['polling']=False
        try:
            self.logger.info(json.dumps({'device_id':device_id,'host':state['host'],'name':state['name'],
                                         **sample},ensure_ascii=False))
        except OSError as exc:
            self.log_warning=str(exc)

    def worker(self,device_id,config):
        try:
            sample=self.sampler(config,self.timeout,self.retries)
            self.apply_sample(device_id,sample)
        except Exception as exc:
            self.apply_sample(device_id,{'observed_at':utc_now(),'ping_ok':False,'ping_ms':None,
                'snmp_online':False,'complete':False,'received_count':0,'rows':[],
                'diagnostics':None,'error':'Tester error: '+str(exc),'http_error':'',
                'mac':'','inverter_status':'Unverified','data_fresh':False})
        finally:
            with self.lock:
                self.active.discard(device_id)
                if device_id in self.states:
                    self.states[device_id]['polling']=False
                    self.due[device_id]=time.monotonic()+self.interval
            self.wake.set()

    def scheduler(self):
        while not self.stop_event.is_set():
            self.wake.clear()
            with self.lock:
                now=time.monotonic()
                eligible=sorted((key for key in self.configs if not self.configs[key]['paused']
                                 and key not in self.active and self.due[key]<=now),key=lambda key:self.due[key])
                for key in eligible[:max(0,self.workers-len(self.active))]:
                    self.active.add(key)
                    self.states[key]['polling']=True
                    self.pool.submit(self.worker,key,self.configs[key].copy())
            self.wake.wait(0.25)

    def start(self):
        self.thread=threading.Thread(target=self.scheduler,daemon=True,name='fleet-scheduler')
        self.thread.start()

    def close(self):
        self.stop_event.set()
        self.wake.set()
        if hasattr(self,'thread'): self.thread.join(timeout=2)
        self.pool.shutdown(wait=True,cancel_futures=True)
        for handler in self.logger.handlers[:]:
            handler.close()
            self.logger.removeHandler(handler)

    def snapshot(self):
        with self.lock:
            devices=copy.deepcopy(list(self.states.values()))
            now=time.time()
            mac_hosts={}
            for state in devices:
                state['last_ok_age']=round(now-state['last_ok_epoch'],1) if state['last_ok_epoch'] else None
                if state.get('mac'):
                    mac_hosts.setdefault(state['mac'],[]).append(state['host'])
            for state in devices:
                state['duplicate_mac']=bool(state.get('mac') and len(mac_hosts[state['mac']])>1)
                if state['paused'] or state['last_ok_age'] is not None and state['last_ok_age']>self.interval+25:
                    state['data_fresh']=False
                    state['values_stale']=bool(state['rows'])
                    if state['inverter_status']=='Fresh': state['inverter_status']='Stale'
        return {'version':VERSION,'server_time':utc_now(),'interval':self.interval,'workers':self.workers,
                'log_warning':self.log_warning,'devices':devices,
                'counts':{'total':len(devices),'responding':sum(s['snmp_online'] and not s['paused'] for s in devices),
                          'fresh':sum(s['data_fresh'] for s in devices),
                          'attention':sum(bool(s['error'] or s['duplicate_mac'] or (s['polls'] and not s['complete'])) for s in devices)}}


def safe_csv(value):
    text=str(value if value is not None else '')
    return "'"+text if text.lstrip().startswith(('=','+','-','@')) else text


def export_csv(state):
    output=io.StringIO(newline='')
    columns=['name','host','mac','ping_ok','snmp_online','complete','inverter_status','polls','successes',
             'failures','consecutive_failures','resets_observed','last_ok_at','error','firmware',
             'device_uptime_seconds','free_heap_bytes','rs485_last_good_age_seconds','network_recoveries',
             'arp_address_conflicts','enc_rx_overflows','enc_tx_failures','udp_send_timeouts']
    writer=csv.DictWriter(output,fieldnames=columns)
    writer.writeheader()
    for device in state['devices']:
        row={**device,**(device.get('diagnostics') or {})}
        writer.writerow({key:safe_csv(row.get(key)) for key in columns})
    return output.getvalue().encode('utf-8-sig')


class Handler(BaseHTTPRequestHandler):
    fleet: Fleet
    def log_message(self,*args): pass
    def reply(self,status,body,kind='application/json; charset=utf-8'):
        if not isinstance(body,bytes): body=json.dumps(body,ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type',kind)
        self.send_header('Content-Length',str(len(body)))
        self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff')
        if kind.startswith('text/csv'): self.send_header('Content-Disposition','attachment; filename="snmp_devices.csv"')
        if urlparse(self.path).path=='/api/export.json': self.send_header('Content-Disposition','attachment; filename="snmp_devices.json"')
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        path=urlparse(self.path).path
        if path=='/': self.reply(200,(ROOT/'tools/multi_snmp_dashboard.html').read_bytes(),'text/html; charset=utf-8')
        elif path in ('/api/state','/api/export.json'): self.reply(200,self.fleet.snapshot())
        elif path=='/api/export.csv': self.reply(200,export_csv(self.fleet.snapshot()),'text/csv; charset=utf-8')
        elif path=='/health': self.reply(200,{'ok':True,'version':VERSION})
        else: self.reply(404,{'error':'Not found.'})
    def do_POST(self):
        try:
            origin=self.headers.get('Origin')
            if origin and origin!=f'http://127.0.0.1:{self.server.server_port}':
                self.reply(403,{'error':'Use this app from its local dashboard URL.'})
                return
            length=int(self.headers.get('Content-Length','0'))
            if not 0<length<=100_000: raise ValueError('Request body is empty or too large.')
            data=json.loads(self.rfile.read(length))
            if not isinstance(data,dict): raise ValueError('Request must be a JSON object.')
            path=urlparse(self.path).path
            if path=='/api/devices':
                items=parse_targets(data['text']) if 'text' in data else data.get('devices',[])
                if not isinstance(items,list) or not items: raise ValueError('Add at least one device.')
                result={'added':self.fleet.add(items)}
            elif path=='/api/remove':
                self.fleet.remove(str(data['id'])); result={'removed':True}
            elif path=='/api/pause':
                if not isinstance(data.get('paused'),bool): raise ValueError('Paused must be true or false.')
                self.fleet.pause(str(data['id']),data['paused']); result={'ok':True}
            elif path=='/api/poll':
                self.fleet.poll_now(data.get('id')); result={'ok':True}
            else:
                self.reply(404,{'error':'Not found.'}); return
            self.reply(200,{'ok':True,**result})
        except (ValueError,KeyError,TypeError,OSError) as exc:
            self.reply(400,{'error':str(exc)})


def main():
    parser=argparse.ArgumentParser(description='ETPL multi-device SNMP tester')
    parser.add_argument('--host',action='append',default=[])
    parser.add_argument('--port',type=int,default=8770)
    parser.add_argument('--workers',type=int,default=8)
    parser.add_argument('--interval',type=float,default=7.0)
    parser.add_argument('--timeout',type=float,default=4.0)
    parser.add_argument('--retries',type=int,default=1)
    parser.add_argument('--runtime',type=Path,default=ROOT/'_snmp_multi_runtime')
    parser.add_argument('--no-browser',action='store_true')
    args=parser.parse_args()
    if not 1<=args.workers<=16 or not 2<=args.interval<=3600 or not 0.1<=args.timeout<=10 or not 0<=args.retries<=2:
        parser.error('Workers 1–16; interval 2–3600s; timeout 0.1–10s; retries 0–2.')
    fleet=Fleet(args.runtime,args.workers,args.interval,args.timeout,args.retries)
    try:
        if args.host: fleet.add([{'host':host,'name':'Bench controller' if host=='192.168.2.1' else host} for host in args.host])
        handler=type('FleetHandler',(Handler,),{'fleet':fleet})
        try: server=ThreadingHTTPServer(('127.0.0.1',args.port),handler)
        except OSError: server=ThreadingHTTPServer(('127.0.0.1',0),handler)
        url=f'http://127.0.0.1:{server.server_port}/'
        (args.runtime/'server.json').write_text(json.dumps({'url':url,'version':VERSION},indent=2),encoding='utf-8')
        fleet.start()
        print(f'ETPL SNMP Multi-Device Tester | {VERSION}\nApp: {url}\nSaved devices / logs: {args.runtime}',flush=True)
        if not args.no_browser: webbrowser.open(url)
        try: server.serve_forever()
        except KeyboardInterrupt: pass
        finally: server.server_close()
    finally: fleet.close()
    return 0


if __name__=='__main__': raise SystemExit(main())
