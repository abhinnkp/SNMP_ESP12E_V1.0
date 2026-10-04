#include <stdio.h>
#include <string.h>
#include "uip.h"
#ifndef UIP_SOURCE
#define UIP_SOURCE "uip.c"
#endif
#include UIP_SOURCE
static int udp_calls,tcp_checks,udp_checks;
void uipclient_appcall(void) {}
void uipudp_appcall(void) { ++udp_calls; }
u16_t uip_ipchksum(void) { return 0xffff; }
u16_t uip_tcpchksum(void) { ++tcp_checks; return 0xffff; }
u16_t uip_udpchksum(void) { ++udp_checks; return 0xffff; }

static void packet(int proto,int length) {
  uip_init(); memset(uip_buf,0,sizeof(uip_buf));
  uip_ipaddr(uip_hostaddr,172,16,16,40);
  BUF->vhl=0x45; BUF->proto=proto; BUF->len[0]=length>>8; BUF->len[1]=length;
  uip_ipaddr_copy(BUF->destipaddr,uip_hostaddr);
  uip_ipaddr(BUF->srcipaddr,172,16,17,100); uip_len=length;
  UDPBUF->srcport=HTONS(40000); UDPBUF->destport=HTONS(161);
  UDPBUF->udplen=HTONS(length-20);
  uip_udp_conns[0].lport=HTONS(161);
}
int main(int argc,char **argv) {
  int valid=0;
  if(argc!=2) return 2;
  if(!strcmp(argv[1],"short_udp")) { packet(UIP_PROTO_UDP,20); UDPBUF->udpchksum=1; }
  else if(!strcmp(argv[1],"bad_udp_length")) { packet(UIP_PROTO_UDP,32); UDPBUF->udplen=HTONS(2000); UDPBUF->udpchksum=1; }
  else if(!strcmp(argv[1],"udp_length_underflow")) { packet(UIP_PROTO_UDP,32); UDPBUF->udplen=HTONS(7); UDPBUF->udpchksum=1; }
  else if(!strcmp(argv[1],"short_tcp")) { packet(UIP_PROTO_TCP,20); BUF->tcpoffset=0x50; }
  else if(!strcmp(argv[1],"bad_tcp_offset")) { packet(UIP_PROTO_TCP,40); BUF->tcpoffset=0xF0; }
  else if(!strcmp(argv[1],"short_icmp")) { packet(UIP_PROTO_ICMP,20); ICMPBUF->type=8; }
  else if(!strcmp(argv[1],"valid_udp")) { packet(UIP_PROTO_UDP,32); valid=1; }
  else if(!strcmp(argv[1],"valid_icmp")) { packet(UIP_PROTO_ICMP,28); ICMPBUF->type=8; valid=2; }
  else return 2;
  uip_input();
  if(valid==1 ? udp_calls!=1 : valid==2 ? uip_len!=28 : (udp_calls || udp_checks || tcp_checks || uip_len)) {
    fprintf(stderr,"FAIL %s len=%u udp=%d checksum=%d/%d\n",argv[1],uip_len,udp_calls,udp_checks,tcp_checks); return 1;
  }
  printf("PASS %s\n",argv[1]); return 0;
}
