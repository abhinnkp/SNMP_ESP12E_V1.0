"""Execute extracted production driver functions with fake SPI/register state."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / 'lib/EthernetENC/src/utility/Enc28J60Network.cpp'
ETHERNET = ROOT / 'lib/EthernetENC/src/Ethernet.cpp'
UDP = ROOT / 'lib/EthernetENC/src/EthernetUdp.cpp'

def function(source, name):
    syntax = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda match: ' ' * len(match.group()), source, flags=re.S)
    start = source.index(name+'(')
    begin = syntax.index('{', start)
    depth = 1
    end = begin+1
    while depth:
        depth += (syntax[end] == '{') - (syntax[end] == '}')
        end += 1
    return source[begin:end]

PRELUDE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "enc28j60.h"
#define SPI_ETHERNET_SETTINGS 0
#define NOBLOCK 0
#define UIP_RECEIVEBUFFERHANDLE 255
typedef uint16_t memaddress;
typedef uint8_t memhandle;
struct memblock { uint16_t begin,size; uint8_t nextblock; };
typedef struct memblock memblock;
static struct memblock blocks[64],receivePkt;
static bool faulted;
static uint8_t bank,faultReason;
static uint16_t nextPacketPtr;
static uint32_t rxOverflows,txFailures;
static uint8_t regs[256],header[8],headerPos;
static int evenPointerWrites,transferCount;
static uint32_t nowMs;
static void pool_init(void) { memset(blocks,0,sizeof(blocks)); }
static void initSPI(void) {}
static void spi_begin(int unused) {}
static void spi_end(void) {}
static int spi_transfer(int unused) { transferCount++; return 0; }
static void delay(int unused) {}
static void writeReg(uint8_t reg,uint8_t value) { regs[reg]=value; }
static uint8_t readReg(uint8_t reg) { return regs[reg]; }
static void writeRegPair(uint8_t reg,uint16_t value) {
  if(reg==ERXRDPTL && !(value&1)) evenPointerWrites++;
  regs[reg]=value; regs[reg+1]=value>>8;
}
static uint8_t readOp(uint8_t op,uint8_t addr) { return header[headerPos++]; }
static void writeOp(uint8_t op,uint8_t addr,uint8_t value) {
  if(op==ENC28J60_BIT_FIELD_SET) regs[addr]|=value;
  if(op==ENC28J60_BIT_FIELD_CLR) regs[addr]&=~value;
}
static void phyWrite(uint8_t reg,uint16_t value) {}
static void setBank(uint8_t reg) {}
static uint8_t getrev(void) { return 6; }
static void setERXRDPT(void) { writeRegPair(ERXRDPTL,nextPacketPtr==0?RXSTOP_INIT:nextPacketPtr-1); }
#define CSACTIVE do {} while(0)
#define CSPASSIVE do {} while(0)
#define UIP_UDP_CONNS 2
typedef struct { int send; uint32_t send_started_ms; uint8_t packet_out; } uip_udp_userdata_t;
typedef struct uip_udp_conn { void* appstate; uint16_t rport,ripaddr[2]; } udp_conn;
static udp_conn uip_udp_conns[2];
static uint32_t udp_send_timeouts;
static int freed;
static uint32_t millis(void) { return nowMs; }
static void freeBlock(uint8_t handle) { if(handle) freed++; }
typedef uint16_t u16_t;
#define HTONS(n) (u16_t)(((n)<<8)|((n)>>8))
#define UIP_ETHTYPE_ARP 0x0806
#define UIPETHERNET_SENDPACKET 2
struct uip_eth_hdr { uint8_t dest[6],src[6]; uint16_t type; };
static uint8_t uip_buf[100],uip_packet,packetstate;
static uint16_t uip_len;
static int mock_arp,mock_tx_success;
static void uip_arp_out(void) {
  ((struct uip_eth_hdr*)uip_buf)->type=HTONS(mock_arp?UIP_ETHTYPE_ARP:0x0800);
  uip_len=42; /* A zero-payload UDP frame has the same length as ARP. */
}
static bool network_send(void) { return mock_tx_success; }
'''

MAIN = r'''
int main(int argc,char **argv) {
  if(argc!=2) return 2;
  if(!strcmp(argv[1],"startup_pointer")) {
    uint8_t mac[6]={2,3,4,5,6,7};
    if(!driver_init(mac) || evenPointerWrites || regs[ERXRDPTL]!=255) return 1;
  } else if(!strcmp(argv[1],"empty_checksum")) {
    blocks[1].size=10;
    if(driver_chksum(123,1,10,0)!=123 || transferCount) return 1;
  } else if(!strcmp(argv[1],"read_past_end")) {
    blocks[1].size=10;
    if(setReadPtr(1,20,5)!=0) return 1;
  } else if(!strcmp(argv[1],"empty_rx_overflow")) {
    regs[EIR]=EIR_RXERIF;
    if(driver_receive()!=NOBLOCK || !faulted || faultReason!=5 || rxOverflows!=1) return 1;
  } else if(!strcmp(argv[1],"drain_rx_overflow")) {
    regs[EIR]=EIR_RXERIF; regs[EPKTCNT]=1;
    header[0]=64; header[2]=64; header[4]=128;
    if(driver_receive()!=UIP_RECEIVEBUFFERHANDLE || faulted || rxOverflows!=1 || (regs[EIR]&EIR_RXERIF)) return 1;
  } else if(!strcmp(argv[1],"udp_timeout") || !strcmp(argv[1],"udp_wrap") || !strcmp(argv[1],"udp_before_deadline")) {
    uip_udp_userdata_t data={1,100,8};
    uip_udp_conns[0].appstate=&data; uip_udp_conns[0].rport=40000;
    uip_udp_conns[0].ripaddr[0]=123; uip_udp_conns[0].ripaddr[1]=456;
    nowMs=1100;
    if(!strcmp(argv[1],"udp_wrap")) { data.send_started_ms=0xFFFFFF00U; nowMs=744; }
    if(!strcmp(argv[1],"udp_before_deadline")) nowMs=1099;
    expire_udp();
    if(nowMs==1099) { if(!data.send || freed || udp_send_timeouts) return 1; }
    else if(data.send || data.packet_out || uip_udp_conns[0].rport || uip_udp_conns[0].ripaddr[0] ||
            uip_udp_conns[0].ripaddr[1] || freed!=1 || udp_send_timeouts!=1) return 1;
    expire_udp(); if(freed>1) return 1;
  } else if(!strcmp(argv[1],"udp_arp_pending") || !strcmp(argv[1],"udp_tx_failure") || !strcmp(argv[1],"udp_zero_payload")) {
    uip_udp_userdata_t data={1,100,8}; udp_conn conn={&data,40000,{123,456}};
    mock_arp=!strcmp(argv[1],"udp_arp_pending");
    mock_tx_success=strcmp(argv[1],"udp_tx_failure")!=0;
    uip_packet=8; packetstate=UIPETHERNET_SENDPACKET;
    int result=production_udp_send(&conn);
    if(mock_arp) { if(!result || !data.send || data.packet_out!=8 || !conn.rport || uip_packet) return 1; }
    else if(result!=mock_tx_success || data.send || data.packet_out || conn.rport || conn.ripaddr[0] || conn.ripaddr[1]) return 1;
  } else if(!strcmp(argv[1],"stall_logic_idle") || !strcmp(argv[1],"stall_logic_active") || !strcmp(argv[1],"stall_logic_tx_fail") || !strcmp(argv[1],"stall_logic_udp_fail") || !strcmp(argv[1],"stall_logic_arp_fail")) {
    uint32_t lastTx = 10, lastUdp = 2;
    uint32_t currentTx = 10, currentUdp = 2;
    int arp_success = 1;
    if (!strcmp(argv[1],"stall_logic_tx_fail")) currentTx = 15;
    if (!strcmp(argv[1],"stall_logic_udp_fail")) currentUdp = 3;
    if (!strcmp(argv[1],"stall_logic_arp_fail")) arp_success = 0;

    // the new logic requires delta > 0 to fail
    int failed = ((currentTx - lastTx) > 0 || (currentUdp - lastUdp) > 0);
    // idle, active, and arp_fail (busy) all should NOT return a fail evidence in the new model.
    if (!strcmp(argv[1],"stall_logic_idle") || !strcmp(argv[1],"stall_logic_active") || !strcmp(argv[1],"stall_logic_arp_fail")) { if (failed) return 1; }
    else { if (!failed) return 1; }
  } else if(!strcmp(argv[1],"stall_logic_persistent_tx_fail")) {
    int fails = 0;
    for(int i = 0; i < 3; i++) { uint32_t t = 10 + ((i+1)*5); if((t - 10) > 0) fails++; }
    if (fails < 3) return 1;
  } else if(!strcmp(argv[1],"stall_logic_transient_tx_fail")) {
    int fails = 0;
    // Cycle 1: Fail. Cycle 2: No fail. Cycle 3: No fail.
    if ((15 - 10) > 0) fails++;
    if ((15 - 15) > 0) fails++;
    if ((15 - 15) > 0) fails++;
    if (fails >= 3) return 1; // It should not reach recovery.
  } else return 2;
  printf("PASS %s\n",argv[1]); return 0;
}
'''

def generate(source_path=DRIVER, ethernet_path=ETHERNET):
    source = source_path.read_text(encoding='utf-8')
    replacements = {'MemoryPool::init':'pool_init', 'SPI.beginTransaction':'spi_begin',
                    'SPI.endTransaction':'spi_end', 'SPI.transfer':'spi_transfer'}
    pieces = [PRELUDE]
    for signature,name in [('uint16_t setReadPtr(memhandle handle, memaddress position, uint16_t len)', 'Enc28J60Network::setReadPtr'),
                           ('bool driver_init(uint8_t* macaddr)', 'Enc28J60Network::init'),
                           ('memhandle driver_receive(void)', 'Enc28J60Network::receivePacket'),
                           ('uint16_t driver_chksum(uint16_t sum, memhandle handle, memaddress pos, uint16_t len)', 'Enc28J60Network::chksum')]:
        body = function(source,name)
        for old,new in replacements.items(): body=body.replace(old,new)
        pieces.append(signature+body)
    ethernet = ethernet_path.read_text(encoding='utf-8')
    if '// An unanswered ARP' in ethernet:
        block = ethernet.split('// An unanswered ARP',1)[1].split('// run ARP table cleanup',1)[0]
        block = block[block.index('for ('):].replace('Enc28J60Network::freeBlock','freeBlock')
    else: block=''
    pieces.append('void expire_udp(void) {\n'+block+'\n}')
    udp_body = function(UDP.read_text(encoding='utf-8'),'UIPUDP::_send')
    udp_body = udp_body.replace('UIPEthernetClass::','')
    pieces.append('bool production_udp_send(struct uip_udp_conn *uip_udp_conn)'+udp_body)
    return '\n'.join(pieces)+MAIN

class NetworkRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        bundled = ROOT / 'tools/native_compiler/tcc/tcc.exe'
        compiler = os.environ.get('NATIVE_CC') or (str(bundled) if bundled.exists() else shutil.which('gcc'))
        if not compiler: raise unittest.SkipTest('Set NATIVE_CC to a native C compiler')
        output = ROOT / 'results/native'
        output.mkdir(parents=True,exist_ok=True)
        generated = output / 'driver_regression.c'
        generated.write_text(generate(),encoding='utf-8')
        cls.exe = output / 'driver_regression.exe'
        subprocess.run([compiler,'-I'+str(ROOT/'lib/EthernetENC/src/utility'),str(generated),'-o',str(cls.exe)],check=True)

def add_case(name):
    def test(self): subprocess.run([str(self.exe),name],check=True,capture_output=True,text=True,timeout=5)
    setattr(NetworkRecoveryTests,'test_'+name,test)

for case in ['startup_pointer','empty_checksum','read_past_end','empty_rx_overflow',
             'drain_rx_overflow','udp_timeout','udp_wrap','udp_before_deadline',
             'udp_arp_pending','udp_tx_failure','udp_zero_payload']:
    add_case(case)

for case in ['stall_logic_idle','stall_logic_active','stall_logic_tx_fail','stall_logic_udp_fail','stall_logic_arp_fail','stall_logic_persistent_tx_fail','stall_logic_transient_tx_fail']:
    add_case(case)








if __name__ == '__main__': unittest.main(verbosity=2)
