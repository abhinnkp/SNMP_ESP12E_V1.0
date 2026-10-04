#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uip_arp.h"
u8_t uip_buf[UIP_BUFSIZE+2];
u16_t uip_len;
void *uip_appdata;
uip_ipaddr_t uip_hostaddr, uip_netmask, uip_draddr;
struct uip_eth_addr uip_ethaddr = {{2,232,38,92,222,204}};
#ifndef ARP_SOURCE
#define ARP_SOURCE "uip_arp.c"
#endif
#include ARP_SOURCE

static struct uip_eth_addr old_mac = {{2,1,2,3,4,5}};
static struct uip_eth_addr new_mac = {{2,6,7,8,9,10}};
static void reset(void) {
  memset(uip_buf, 0, sizeof(uip_buf));
  uip_ipaddr(uip_hostaddr,172,16,16,40);
  uip_ipaddr(uip_netmask,255,255,254,0);
  uip_ipaddr(uip_draddr,172,16,16,1);
  uip_arp_init();
}
static int used(void) {
  int k,n=0; for(k=0;k<UIP_ARPTAB_SIZE;k++)
    if(arp_table[k].ipaddr[0] | arp_table[k].ipaddr[1]) n++;
  return n;
}
static void arp_packet(u16_t opcode, u16_t *sender, u16_t *target, struct uip_eth_addr mac) {
  memset(uip_buf,0,sizeof(uip_buf));
  BUF->hwtype=HTONS(1); BUF->protocol=HTONS(UIP_ETHTYPE_IP);
  BUF->hwlen=6; BUF->protolen=4; BUF->opcode=HTONS(opcode);
  BUF->shwaddr=mac; uip_ipaddr_copy(BUF->sipaddr,sender);
  uip_ipaddr_copy(BUF->dipaddr,target); uip_len=42;
}
static int run_case(const char *name) {
  int k; uip_ipaddr_t peer; reset();
  if(!strcmp(name,"wrap_expiry")) {
    arptime=250; uip_arp_update(uip_draddr,&old_mac);
    for(k=0;k<120;k++) uip_arp_timer();
    return used()==0;
  }
  if(!strcmp(name,"wrap_eviction")) {
    arptime=250;
    for(k=0;k<UIP_ARPTAB_SIZE;k++) { uip_ipaddr(peer,172,16,16,k+1); uip_arp_update(peer,&old_mac); }
    for(k=0;k<10;k++) uip_arp_timer();
    uip_arp_update(uip_draddr,&new_mac); /* gateway refreshed after wrap */
    uip_ipaddr(peer,172,16,16,200); uip_arp_update(peer,&old_mac);
    for(k=0;k<UIP_ARPTAB_SIZE;k++) if(uip_ipaddr_cmp(arp_table[k].ipaddr,uip_draddr)) return 1;
    return 0;
  }
  if(!strcmp(name,"halfword_zero")) {
    uip_ipaddr(peer,10,0,0,0);
    uip_arp_update(peer,&old_mac); uip_arp_update(peer,&new_mac);
    return used()==1 && !memcmp(arp_table[0].ethaddr.addr,new_mac.addr,6);
  }
  if(!strcmp(name,"gateway_announcement")) {
    uip_arp_update(uip_draddr,&old_mac);
    arp_packet(ARP_REQUEST,uip_draddr,uip_draddr,new_mac);
    uip_arp_arpin();
    return !memcmp(arp_table[0].ethaddr.addr,new_mac.addr,6) && !uip_len;
  }
  if(!strcmp(name,"gateway_reply_announcement")) {
    uip_arp_update(uip_draddr,&old_mac);
    arp_packet(ARP_REPLY,uip_draddr,uip_draddr,new_mac);
    uip_arp_arpin();
    return !memcmp(arp_table[0].ethaddr.addr,new_mac.addr,6);
  }
  if(!strcmp(name,"invalid_arp")) {
    arp_packet(ARP_REQUEST,uip_draddr,uip_hostaddr,old_mac);
    BUF->hwlen=5; uip_arp_arpin(); return !used() && !uip_len;
  }
  if(!strcmp(name,"short_ip")) {
    IPBUF->srcipaddr[0]=uip_draddr[0]; IPBUF->srcipaddr[1]=uip_draddr[1];
    IPBUF->ethhdr.src=old_mac; uip_len=14;
    uip_arp_ipin(); return !used() && !uip_len;
  }
  if(!strcmp(name,"init_clock")) { arptime=200; uip_arp_init(); return arptime==0; }
  if(!strcmp(name,"duplicate_ip")) {
    arp_packet(ARP_REQUEST,uip_hostaddr,uip_hostaddr,new_mac);
    uip_arp_arpin(); return !used() && !uip_len;
  }
  if(!strcmp(name,"site_23")) {
    uip_ipaddr(peer,172,16,17,100);
    IPBUF->srcipaddr[0]=peer[0]; IPBUF->srcipaddr[1]=peer[1];
    IPBUF->ethhdr.src=old_mac; uip_len=34; uip_arp_ipin();
    return used()==1;
  }
  if(!strcmp(name,"routed_reply")) {
    uip_arp_update(uip_draddr,&old_mac);
    uip_ipaddr(IPBUF->destipaddr,172,16,0,100); uip_len=28;
    uip_arp_out(); return uip_len==42 && IPBUF->ethhdr.type==HTONS(UIP_ETHTYPE_IP)
      && !memcmp(IPBUF->ethhdr.dest.addr,old_mac.addr,6);
  }
#ifndef LEGACY
  if(!strcmp(name,"announcement_format")) {
    uip_arp_request(uip_hostaddr);
    return uip_len==42 && BUF->opcode==HTONS(ARP_REQUEST) && BUF->hwtype==HTONS(1)
      && uip_ipaddr_cmp(BUF->sipaddr,uip_hostaddr) && uip_ipaddr_cmp(BUF->dipaddr,uip_hostaddr)
      && !memcmp(BUF->ethhdr.dest.addr,broadcast_ethaddr.addr,6);
  }
  if(!strcmp(name,"gateway_request")) {
    uip_arp_request(uip_draddr);
    return uip_ipaddr_cmp(BUF->dipaddr,uip_draddr) && uip_ipaddr_cmp(BUF->sipaddr,uip_hostaddr);
  }
#endif
  return 0;
}
int main(int argc,char **argv) {
  if(argc!=2) return 2;
  if(!run_case(argv[1])) { fprintf(stderr,"FAIL %s\n",argv[1]); return 1; }
  printf("PASS %s\n",argv[1]); return 0;
}
