
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

uint16_t setReadPtr(memhandle handle, memaddress position, uint16_t len){
  memblock *packet = handle == UIP_RECEIVEBUFFERHANDLE ? &receivePkt : &blocks[handle];
  if (position >= packet->size) return 0;
  memaddress start = handle == UIP_RECEIVEBUFFERHANDLE && packet->begin + position > RXSTOP_INIT ? packet->begin + position-((RXSTOP_INIT + 1)-RXSTART_INIT) : packet->begin + position;

  writeRegPair(ERDPTL, start);

  if (len > packet->size - position)
    len = packet->size - position;
  return len;
}
bool driver_init(uint8_t* macaddr){
  faulted = false;
  faultReason = 0;
  bank = 0xff;
  pool_init(); // 1 byte in between RX_STOP_INIT and pool to allow prepending of controlbyte

  initSPI();

  spi_begin(SPI_ETHERNET_SETTINGS);

  // perform system reset
  writeOp(ENC28J60_SOFT_RESET, 0, ENC28J60_SOFT_RESET);
  delay(50);
  // check CLKRDY bit to see if reset is complete
  // The CLKRDY does not work. See Rev. B4 Silicon Errata point. Just wait.
  //while(!(readReg(ESTAT) & ESTAT_CLKRDY));
  // do bank 0 stuff
  // initialize receive buffer
  // 16-bit transfers, must write low byte first
  // set receive buffer start address
  nextPacketPtr = RXSTART_INIT;
  // Rx start
  writeRegPair(ERXSTL, RXSTART_INIT);
  // set receive pointer address
  // Errata 14 applies at startup too: every ERXRDPT write must be odd.
  writeRegPair(ERXRDPTL, RXSTOP_INIT);
  // RX end
  writeRegPair(ERXNDL, RXSTOP_INIT);
  // TX start
  //writeRegPair(ETXSTL, TXSTART_INIT);
  // TX end
  //writeRegPair(ETXNDL, TXSTOP_INIT);
  // do bank 1 stuff, packet filter:
  // For broadcast packets we allow only ARP packtets
  // All other packets should be unicast only for our mac (MAADR)
  //
  // The pattern to match on is therefore
  // Type     ETH.DST
  // ARP      BROADCAST
  // 06 08 -- ff ff ff ff ff ff -> ip checksum for theses bytes=f7f9
  // in binary these poitions are:11 0000 0011 1111
  // This is hex 303F->EPMM0=0x3f,EPMM1=0x30
  /* Direct cable discovery begins with a broadcast ARP request.  The pattern
     matcher alone did not accept that request on the installed B6/B7 module.
     Keep the ARP pattern and explicitly admit broadcasts; uIP will discard
     unrelated broadcast protocols after the frame is read. */
  writeReg(ERXFCON, ERXFCON_UCEN|ERXFCON_CRCEN|ERXFCON_PMEN|ERXFCON_BCEN);
  writeRegPair(EPMM0, 0x303f);
  writeRegPair(EPMCSL, 0xf7f9);
  //
  //
  // do bank 2 stuff
  // enable MAC receive
  // and bring MAC out of reset (writes 0x00 to MACON2)
  writeRegPair(MACON1, MACON1_MARXEN|MACON1_TXPAUS|MACON1_RXPAUS);
  // enable automatic padding to 60bytes and CRC operations
  writeOp(ENC28J60_BIT_FIELD_SET, MACON3, MACON3_PADCFG0|MACON3_TXCRCEN|MACON3_FRMLNEN);
  // set inter-frame gap (non-back-to-back)
  writeRegPair(MAIPGL, 0x0C12);
  // set inter-frame gap (back-to-back)
  writeReg(MABBIPG, 0x12);
  // Set the maximum packet size which the controller will accept
  // Do not send packets longer than MAX_FRAMELEN:
  writeRegPair(MAMXFLL, MAX_FRAMELEN);
  // do bank 3 stuff
  // write MAC address
  // NOTE: MAC address in ENC28J60 is byte-backward
  writeReg(MAADR5, macaddr[0]);
  writeReg(MAADR4, macaddr[1]);
  writeReg(MAADR3, macaddr[2]);
  writeReg(MAADR2, macaddr[3]);
  writeReg(MAADR1, macaddr[4]);
  writeReg(MAADR0, macaddr[5]);
  // no loopback of transmitted frames
  phyWrite(PHCON2, PHCON2_HDLDIS);
  // switch to bank 0
  setBank(ECON1);
  // enable interrutps
  writeOp(ENC28J60_BIT_FIELD_SET, EIE, EIE_INTIE|EIE_PKTIE);
  // enable packet reception
  writeOp(ENC28J60_BIT_FIELD_SET, ECON1, ECON1_RXEN);
  //Configure leds
  phyWrite(PHLCON,0x476);

  spi_end();

  return getrev() && !faulted;
}
memhandle driver_receive(void){
  if (faulted) return NOBLOCK;
  uint8_t rxstat;
  uint16_t len;

  spi_begin(SPI_ETHERNET_SETTINGS);

  // check if a packet has been received and buffered
  //if( !(readReg(EIR) & EIR_PKTIF) ){
  // The above does not work. See Rev. B4 Silicon Errata point 6.
  uint8_t pending = readReg(EPKTCNT);
  if (readReg(EIR) & EIR_RXERIF) {
    ++rxOverflows;
    writeOp(ENC28J60_BIT_FIELD_CLR, EIR, EIR_RXERIF);
    // Drain a full queue normally. An empty queue with RXERIF can be a stalled ring.
    if (!pending) {
      faulted = true;
      faultReason = 5;
      spi_end();
      return NOBLOCK;
    }
  }
  if (pending != 0)
    {
      uint16_t readPtr = nextPacketPtr+6 > RXSTOP_INIT ? nextPacketPtr+6-((RXSTOP_INIT + 1)-RXSTART_INIT) : nextPacketPtr+6;
      // Set the read pointer to the start of the received packet
      writeRegPair(ERDPTL, nextPacketPtr);
      // read the next packet pointer
      nextPacketPtr = readOp(ENC28J60_READ_BUF_MEM, 0);
      nextPacketPtr |= readOp(ENC28J60_READ_BUF_MEM, 0) << 8;
      // read the packet length (see datasheet page 43)
      len = readOp(ENC28J60_READ_BUF_MEM, 0);
      len |= readOp(ENC28J60_READ_BUF_MEM, 0) << 8;
      len -= 4; //remove the CRC count
      // read the receive status (see datasheet page 43)
      rxstat = readOp(ENC28J60_READ_BUF_MEM, 0);
      // Corrupt SPI/ring headers must not become out-of-range buffer handles.
      if (nextPacketPtr > RXSTOP_INIT || (nextPacketPtr & 1) ||
          len < 14 || len > MAX_FRAMELEN) {
        faulted = true;
        faultReason = 3;
        spi_end();
        return NOBLOCK;
      }
      //rxstat |= readOp(ENC28J60_READ_BUF_MEM, 0) << 8;
#ifdef ENC28J60DEBUG
      Serial.print("receivePacket [");
      Serial.print(readPtr,HEX);
      Serial.print("-");
      Serial.print((readPtr+len) % (RXSTOP_INIT+1),HEX);
      Serial.print("], next: ");
      Serial.print(nextPacketPtr,HEX);
      Serial.print(", stat: ");
      Serial.print(rxstat,HEX);
      Serial.print(", count: ");
      Serial.print(readReg(EPKTCNT));
      Serial.print(" -> ");
      Serial.println((rxstat & 0x80)!=0 ? "OK" : "failed");
#endif
      // decrement the packet counter indicate we are done with this packet
      writeOp(ENC28J60_BIT_FIELD_SET, ECON2, ECON2_PKTDEC);
      // check CRC and symbol errors (see datasheet page 44, table 7-3):
      // The ERXFCON.CRCEN is set by default. Normally we should not
      // need to check this.
      if ((rxstat & 0x80) != 0)
        {
          receivePkt.begin = readPtr;
          receivePkt.size = len;
          spi_end();
          return UIP_RECEIVEBUFFERHANDLE;
        }
      // Move the RX read pointer to the start of the next received packet
      // This frees the memory we just read out
      setERXRDPT();
    }
  spi_end();
  return (NOBLOCK);
}
uint16_t driver_chksum(uint16_t sum, memhandle handle, memaddress pos, uint16_t len){
  uint16_t t;
  spi_begin(SPI_ETHERNET_SETTINGS);
  len = setReadPtr(handle, pos, len);
  if (!len) { spi_end(); return sum; }
  --len;
  CSACTIVE;
  // issue read command
  spi_transfer(ENC28J60_READ_BUF_MEM);
  uint16_t i;
  for (i = 0; i < len; i+=2)
  {
    // read data
    t = spi_transfer(0x00) << 8;
    t += spi_transfer(0x00);
    sum += t;
    if(sum < t) {
      sum++;            /* carry */
    }
  }
  if(i == len) {
    t = (spi_transfer(0x00) << 8) + 0;
    sum += t;
    if(sum < t) {
      sum++;            /* carry */
    }
  }
  CSPASSIVE;
  spi_end();

  /* Return sum in host byte order. */
  return sum;
}
void expire_udp(void) {
for (int i = 0; i < UIP_UDP_CONNS; ++i) {
    uip_udp_userdata_t* data = (uip_udp_userdata_t*)uip_udp_conns[i].appstate;
    if (data && data->send &&
        (uint32_t)(millis() - data->send_started_ms) >= 1000UL) {
      freeBlock(data->packet_out);
      data->packet_out = NOBLOCK;
      data->send = false;
      uip_udp_conns[i].rport = 0;
      uip_udp_conns[i].ripaddr[0] = uip_udp_conns[i].ripaddr[1] = 0;
      ++udp_send_timeouts;
    }
  }


}
bool production_udp_send(struct uip_udp_conn *uip_udp_conn){

  uip_arp_out(); //add arp
  if (((struct uip_eth_hdr*)uip_buf)->type == HTONS(UIP_ETHTYPE_ARP))
    {
      uip_packet = NOBLOCK;
      packetstate &= ~UIPETHERNET_SENDPACKET;
#ifdef UIPETHERNET_DEBUG_UDP
      Serial.println(F("udp, uip_poll results in ARP-packet"));
#endif
      return network_send();
    }
  else
  //arp found ethaddr for ip (otherwise packet is replaced by arp-request)
    {
      uip_udp_userdata_t* data = (uip_udp_userdata_t *)(uip_udp_conn->appstate);
      bool sent = network_send();
      data->send = false;
      data->packet_out = NOBLOCK;

      // [J.A] a listening UDP port in uIP filters received messages
      // if rport and ripaddr are set. so we better clear them
      uip_udp_conn->rport = 0;
      uip_udp_conn->ripaddr[0] = 0;
      uip_udp_conn->ripaddr[1] = 0;

#ifdef UIPETHERNET_DEBUG_UDP
      Serial.print(F("udp, uip_packet to send: "));
      Serial.println(uip_packet);
#endif
      return sent;
    }
}
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
    int failed = (!arp_success || (currentTx - lastTx) > 0 || (currentUdp - lastUdp) > 0);
    if (!strcmp(argv[1],"stall_logic_idle") || !strcmp(argv[1],"stall_logic_active")) { if (failed) return 1; }
    else { if (!failed) return 1; }
  } else return 2;
  printf("PASS %s\n",argv[1]); return 0;
}
