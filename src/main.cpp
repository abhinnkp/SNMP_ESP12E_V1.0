/******************************************************************************
 * main.c – Inverter Monitoring System (SNMP Agent)
 ******************************************************************************/

#include <EthernetENC.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

/* Define SIMULATE_INVERTER to test the UI and network services without
  a physical inverter. This populates fake readings so `/api/inverter`
  and SNMP/TCP endpoints can be exercised. For production with the
  real inverter, the simulation should be disabled. */
/* #define SIMULATE_INVERTER */

/* #define DEBUG_ENABLED */
/* Leave this disabled on hardware where Serial is shared with RS-485.
   A full SNMP walk can generate many responses; hex-dumping each packet at
   9600 baud keeps the UART busy long enough to disturb Modbus reads. */
/* #define SNMP_HEX_DUMP_ENABLED */
#ifdef DEBUG_ENABLED
  #define DBG(x)    Serial.print(x)
  #define DBGLN(x)  Serial.println(x)
#else
  #define DBG(x)
  #define DBGLN(x)
#endif

#define RS485_DE_PIN  16
#define FIRMWARE_BUILD_LABEL "SNMP_SITE_RECOVERY_20260930_R3"
/* ENC28J60 SPI wiring for ESP8266:
   SCK=GPIO14 (D5), MISO=GPIO12 (D6), MOSI=GPIO13 (D7), CS=GPIO15 (D8).
   Change only this value if the ENC28J60 CS wire is connected elsewhere. */
#define ETHERNET_CS_PIN 15
byte      gMac[6];
IPAddress gDeviceIP;
String    gMacID  = "";
String    gApSSID = "";
/* EthernetENC is the maintained, SPI-transaction-safe successor to
   UIPEthernet.  The v13 SNMP parser, settings UI, unique MAC and background
   inverter polling remain unchanged. */
EthernetUDP gUdp;
EthernetServer gEthernetWebServer(80);
/* RMS/slave protocol from the TCP specification.  This is intentionally a
   separate listener from the browser setup page on port 80. */
EthernetServer gRmsTcpServer(5555);
static bool          gEthernetReady = false;
static bool          gEthernetWebStarted = false;
static bool          gSnmpUdpReady = false;
static bool          gWiFiAPStarted = false;
static bool          gWiFiAPWindowExpired = false;
static unsigned long gLastEthernetAttemptMs = 0;
static unsigned long gLastWiFiAPAttemptMs = 0;
static unsigned long gWiFiAPStartedMs = 0;
static unsigned long gSnmpUdpRestartAtMs = 0;
static unsigned long gEthernetLinkDownSinceMs = 0;
static bool          gServicingNetwork = false;
static uint32_t      gSnmpRequests = 0;
static uint32_t      gSnmpRejected = 0;
static uint32_t      gNetworkRecoveries = 0;
static uint32_t      gMaxNetworkGapMs = 0;
static uint32_t      gLastNetworkServiceMs = 0;
static uint32_t      gLastLinkCheckMs = 0;
static EthernetLinkStatus gLinkState = Unknown;
static uint8_t       gAddressAnnouncesLeft = 0;
static uint32_t      gNextAddressAnnounceMs = 0;
static uint32_t      gAddressAnnouncements = 0;
static uint32_t      gLinkTransitions = 0;
static uint8_t       gLastNetworkFault = 0;

static uint32_t      gLastTxFailures = 0;
static uint32_t      gLastUdpTimeouts = 0;
static uint32_t      gNextActiveProbeMs = 0;
static uint8_t       gActiveProbeFailures = 0;
#define ETHERNET_RETRY_MS 15000UL
#define ETHERNET_LINK_DOWN_GRACE_MS 30000UL
#define SNMP_UDP_RETRY_MS 15000UL
#define INITIAL_ETHERNET_DELAY_MS 0UL
#define WIFI_AP_RETRY_MS 5000UL
/* The setup hotspot is intentionally temporary.  Press Reset/reboot the
   device to open this configuration window again. */
#define WIFI_AP_CONFIG_WINDOW_MS 600000UL /* 10 minutes */

/* Settings are kept in flash so they survive a reset or power loss. */
#define SETTINGS_MAGIC 0x494E5639UL
#define SETTINGS_EEPROM_SIZE 256
#define DEFAULT_AP_PASSWORD "Tangent123"
#define MODBUS_TIMEOUT_DEFAULT_MS  800U
#define MODBUS_TIMEOUT_MIN_MS      800U
#define MODBUS_TIMEOUT_MAX_MS     1500U
struct DeviceSettings {
  uint32_t magic;
  char apPassword[65];
  bool useStaticIP;
  uint8_t ip[4];
  uint8_t gateway[4];
  uint8_t subnet[4];
  uint8_t dns[4];
  /* Kept at the end so saved settings from the previous firmware layout can
     be migrated safely by loadSettings(). */
  uint16_t modbusTimeoutMs;
};
static DeviceSettings gSettings;
static ESP8266WebServer gWiFiWebServer(80);
static DNSServer gDnsServer;
#define SNMP_PORT  161
#define RX_BUF_SZ  512
#define TX_BUF_SZ  950
static uint8_t sRxBuf[RX_BUF_SZ];
static uint8_t sTxBuf[TX_BUF_SZ];
static uint8_t sVbBuf[750];
static uint8_t sPduBuf[800];
static uint8_t sInnerBuf[880];
struct InverterData {
  uint16_t Output_Voltage_Raw;
  uint16_t Output_Frequency_Raw;
  uint16_t Output_Current_Raw;
  uint16_t Output_Power_Raw;
  uint16_t Input_Voltage_Raw;
  uint16_t Input_Frequency_Raw;
  uint16_t Battery_Voltage_Raw;
  uint16_t Battery_Charging_Current_Raw;
  uint16_t Solar_Voltage_Raw;
  uint16_t Solar_Current_Raw;
  uint16_t Solar_Power_Raw;
  uint16_t Output_Energy_kWh;
  uint16_t Output_Energy_Rem;
  uint16_t Solar_Energy_kWh;
  uint16_t Solar_Energy_Rem;
  uint16_t Flash_Writes;
  uint16_t Uptime_Minutes_k;
  uint16_t Uptime_Minutes_Rem;
  uint16_t Temperature_Raw;
  uint16_t Inverter_Status_Fault;
  uint16_t PFC_Charger_Status_Fault;
  uint16_t MPPT_Charger_Status_Fault;
  uint16_t Mains_Grid_Status_Fault;
  uint16_t Battery_Status_Fault;
};
static InverterData  gInv;
static bool          gDataValid  = false;
static unsigned long gLastReadMs = 0;
static unsigned long gLastGoodReadMs = 0;
static uint8_t       gConsecutiveReadFailures = 0;
#define DATA_CACHE_MS  5000UL
/* Keep a current reading even when no SNMP/TCP client is connected. */
#define INVERTER_POLL_MS 5000UL
#define MODBUS_RETRY_GAP_MS 100UL
#define LAST_GOOD_RETENTION_MS 30000UL
static unsigned long gLastPollMs = 0;
/* This is the only SNMP OID configuration.  The values are exposed as
   children of this node, e.g. <base>.1 through <base>.23.  Keeping the
   base separate from the values makes a v1 GETNEXT walk start at the node
   itself rather than requiring a GET for any child (notably .23). */
#define SNMP_BASE_OID "1.3.6.1.4.1.12345.1.23"
#define MAX_BASE_OID_LEN 29
static uint8_t sBaseOid[MAX_BASE_OID_LEN];
static uint8_t sBaseOidLen = 0;
#define NUM_OIDS    23
/* BER encoding of 1.3.6.1.2.1.1.1.0 (SNMPv2-MIB::sysDescr.0).  Providing
   this standard scalar makes a normal SNMPv2c health check useful, while
   the inverter values remain under SNMP_BASE_OID. */
static const uint8_t kSysDescrOid[] = { 0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00 };
void     loadSettings(void);
void     makeUniqueEthernetMac(void);
bool     startEthernet(void);
void     handleEthernetWeb(void);
void     handleRmsTcp(void);
void     queueSnmpUdpRestart(void);
void     serviceSnmpUdpRestart(void);
void     startWiFiSetupAP(void);
void     handleWiFiConfigPage(void);
void     handleWiFiSaveConfig(void);
void     handleWiFiInverterApi(void);
void     redirectCaptivePortal(void);
void     refreshInverterData(bool force = false);
void     pollInverter(void);
void     serviceNetworkDuringWait(void);
void     cooperativeDelay(uint32_t durationMs);
template<typename UdpType> void handleSNMP(UdpType& udp);
int      buildSnmpResponse(const uint8_t* req, int reqLen, uint8_t* rsp, int rspMax);
bool     MODBUS_READ_ALL_REGISTERS(InverterData* data);
uint16_t crc_cal_value(uint8_t* d, uint8_t len);
void     rs485_transmit_enable(void);
void     rs485_receive_enable(void);
bool     configureSnmpBaseOid(const char* dottedOid);

static int berWLen(uint8_t* b, int len) {
  if (len < 128) { b[0]=(uint8_t)len; return 1; }
  if (len < 256) { b[0]=0x81; b[1]=(uint8_t)len; return 2; }
  b[0]=0x82; b[1]=(uint8_t)(len>>8); b[2]=(uint8_t)(len&0xFF); return 3;
}

static int berRLen(const uint8_t* b, int bLen, int* off) {
  if (*off >= bLen) return -1;
  uint8_t f = b[(*off)++];
  if (f < 0x80) return (int)f;
  int n = f & 0x7F;
  if (n==0 || n>3 || *off+n>bLen) return -1;
  int v = 0;
  while (n--) v = (v<<8) | b[(*off)++];
  return v;
}

static int berReadField(const uint8_t* b, int end, int* pos, uint8_t tag) {
  if (*pos < 0 || *pos >= end || b[(*pos)++] != tag) return -1;
  int len = berRLen(b, end, pos);
  return len >= 0 && len <= end - *pos ? len : -1;
}

static int berWOid(uint8_t* b, uint8_t leaf) {
  const uint8_t oidLen = sBaseOidLen + 1;
  b[0]=0x06; b[1]=oidLen;
  memcpy(b+2, sBaseOid, sBaseOidLen);
  b[2+sBaseOidLen]=leaf;
  return 2+oidLen;
}

static int berWRawOid(uint8_t* b, const uint8_t* oid, int oidLen) {
  if (oidLen < 0 || oidLen > 127) return -1;
  b[0] = 0x06;
  b[1] = (uint8_t)oidLen;
  memcpy(b + 2, oid, oidLen);
  return oidLen + 2;
}

/* Convert the one dotted-decimal setting to SNMP's BER OID bytes.  This
   keeps the implementation independent of how many bytes an OID arc needs. */
bool configureSnmpBaseOid(const char* dottedOid) {
  uint32_t arcs[16];
  uint8_t arcCount = 0;
  uint32_t value = 0;
  bool hasDigit = false;

  for (const char* p = dottedOid;; ++p) {
    if (*p >= '0' && *p <= '9') {
      if (value > 429496729UL) return false;
      value = value * 10UL + (uint32_t)(*p - '0');
      hasDigit = true;
      continue;
    }
    if (*p != '.' && *p != '\0') return false;
    if (!hasDigit || arcCount >= 16) return false;
    arcs[arcCount++] = value;
    value = 0;
    hasDigit = false;
    if (*p == '\0') break;
  }
  if (arcCount < 2 || arcs[0] > 2 || (arcs[0] < 2 && arcs[1] > 39)) return false;

  uint8_t out = 0;
  for (uint8_t i = 0; i < arcCount; ++i) {
    uint32_t subId = (i == 0) ? arcs[0] * 40UL + arcs[1]
                              : (i == 1) ? UINT32_MAX : arcs[i];
    if (i == 1) continue;
    uint8_t encoded[5];
    uint8_t count = 0;
    do { encoded[count++] = (uint8_t)(subId & 0x7F); subId >>= 7; } while (subId && count < sizeof(encoded));
    if (subId || out + count > sizeof(sBaseOid)) return false;
    while (count) {
      uint8_t byte = encoded[--count];
      if (count) byte |= 0x80;
      sBaseOid[out++] = byte;
    }
  }
  sBaseOidLen = out;
  return true;
}

static int berWUint(uint8_t* b, uint8_t tag, uint32_t val) {
  uint8_t v[5]; int vl;
  if (tag == 0x02) {
    if      (val > 0x7FFFFF)  { v[0]=(val>>24)&0xFF;v[1]=(val>>16)&0xFF;v[2]=(val>>8)&0xFF;v[3]=val&0xFF;vl=4; }
    else if (val > 0x7FFF)    { v[0]=(val>>16)&0xFF;v[1]=(val>>8)&0xFF;v[2]=val&0xFF;vl=3; }
    else if (val > 0x7F)      { v[0]=(val>>8)&0xFF;v[1]=val&0xFF;vl=2; }
    else                      { v[0]=val&0xFF;vl=1; }
  } else {
    if      (val > 0x00FFFFFF) { v[0]=(val>>24)&0xFF;v[1]=(val>>16)&0xFF;v[2]=(val>>8)&0xFF;v[3]=val&0xFF;vl=4; }
    else if (val > 0x0000FFFF) { v[0]=(val>>16)&0xFF;v[1]=(val>>8)&0xFF;v[2]=val&0xFF;vl=3; }
    else if (val > 0x000000FF) { v[0]=(val>>8)&0xFF;v[1]=val&0xFF;vl=2; }
    else                       { v[0]=val&0xFF;vl=1; }
  }
  /* BER INTEGER contents are signed even for SNMP's unsigned application
     types. Prefix values whose high bit is set so 228 is encoded as 00 E4,
     not decoded as -28 / 4294967268 by Net-SNMP. */
  if (v[0]&0x80) { memmove(v+1,v,vl);v[0]=0x00;vl++; }
  b[0]=tag;
  int p=1+berWLen(b+1,vl);
  memcpy(b+p,v,vl);
  return p+vl;
}

static int berWStr(uint8_t* b, const char* s, int sLen) {
  b[0]=0x04;
  int p=1+berWLen(b+1,sLen);
  memcpy(b+p,s,sLen);
  return p+sLen;
}

static bool isStrLeaf(uint8_t leaf) { return leaf==1 || leaf==23; }

static void buildFullDataString(char* buf, int maxLen) {
  if (!gDataValid) { strncpy(buf,"$NO_DATA#",maxLen); return; }
  char macHex[13];
  snprintf(macHex,sizeof(macHex),"%02X%02X%02X%02X%02X%02X",
           gMac[0],gMac[1],gMac[2],gMac[3],gMac[4],gMac[5]);
  float OV  = (float)gInv.Output_Voltage_Raw;
  float OF  = gInv.Output_Frequency_Raw   / 10.0f;
  float OC  = gInv.Output_Current_Raw     / 10.0f;
  float OP  = (float)gInv.Output_Power_Raw;
  float IV  = (float)gInv.Input_Voltage_Raw;
  float IF_ = gInv.Input_Frequency_Raw    / 10.0f;
  float BV  = gInv.Battery_Voltage_Raw    / 10.0f;
  float BC  = gInv.Battery_Charging_Current_Raw / 10.0f;
  float SV  = gInv.Solar_Voltage_Raw      / 10.0f;
  float SC  = gInv.Solar_Current_Raw      / 10.0f;
  float SP  = (float)gInv.Solar_Power_Raw;
  float T   = gInv.Temperature_Raw        / 10.0f;
  unsigned long OE = (unsigned long)gInv.Output_Energy_kWh*1000 + gInv.Output_Energy_Rem;
  unsigned long SE = (unsigned long)gInv.Solar_Energy_kWh *1000 + gInv.Solar_Energy_Rem;
  unsigned long UT = (unsigned long)gInv.Uptime_Minutes_k *1000 + gInv.Uptime_Minutes_Rem;
  snprintf(buf,maxLen,
    "$%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
    "%lu,%lu,%u,%lu,%.1f,%u,%u,%u,%u,%u#",
    macHex, OV,OF,OC,OP,IV,IF_,BV,BC,SV,SC,SP,
    OE,SE,gInv.Flash_Writes,UT,T,
    gInv.Inverter_Status_Fault, gInv.PFC_Charger_Status_Fault,
    gInv.MPPT_Charger_Status_Fault, gInv.Mains_Grid_Status_Fault,
    gInv.Battery_Status_Fault);
}

static void getStrVal(uint8_t leaf, char* buf, int maxLen) {
  if (leaf==1)
    snprintf(buf,maxLen,"%02X%02X%02X%02X%02X%02X",
             gMac[0],gMac[1],gMac[2],gMac[3],gMac[4],gMac[5]);
  else if (leaf==23)
    buildFullDataString(buf,maxLen);
}

static uint32_t getNumVal(uint8_t leaf) {
  if (!gDataValid) return 0;
  switch(leaf) {
    case  2: return gInv.Output_Voltage_Raw;
    case  3: return gInv.Output_Frequency_Raw;
    case  4: return gInv.Output_Current_Raw;
    case  5: return gInv.Output_Power_Raw;
    case  6: return gInv.Input_Voltage_Raw;
    case  7: return gInv.Input_Frequency_Raw;
    case  8: return gInv.Battery_Voltage_Raw;
    case  9: return gInv.Battery_Charging_Current_Raw;
    case 10: return gInv.Solar_Voltage_Raw;
    case 11: return gInv.Solar_Current_Raw;
    case 12: return gInv.Solar_Power_Raw;
    case 13: return (uint32_t)gInv.Output_Energy_kWh*1000 + gInv.Output_Energy_Rem;
    case 14: return (uint32_t)gInv.Solar_Energy_kWh *1000 + gInv.Solar_Energy_Rem;
    case 15: return gInv.Flash_Writes;
    case 16: return (uint32_t)gInv.Uptime_Minutes_k *1000 + gInv.Uptime_Minutes_Rem;
    case 17: return gInv.Temperature_Raw;
    case 18: return gInv.Inverter_Status_Fault;
    case 19: return gInv.PFC_Charger_Status_Fault;
    case 20: return gInv.MPPT_Charger_Status_Fault;
    case 21: return gInv.Mains_Grid_Status_Fault;
    case 22: return gInv.Battery_Status_Fault;
    default: return 0;
  }
}

static uint8_t leafTag(uint8_t leaf) {
  if (leaf==13||leaf==14||leaf==15) return 0x41;
  if (leaf==16)                     return 0x43;
  if (leaf>=18&&leaf<=22)           return 0x02;
  return 0x42;
}

static uint8_t matchLeaf(const uint8_t* oid, int oidLen) {
  if (oidLen != sBaseOidLen + 1) return 0;
  if (memcmp(oid, sBaseOid, sBaseOidLen) != 0) return 0;
  uint8_t leaf = oid[sBaseOidLen];
  return (leaf>=1 && leaf<=NUM_OIDS) ? leaf : 0;
}

static int oidCmp(const uint8_t* a, int aLen, const uint8_t* b, int bLen) {
  int minLen = (aLen<bLen) ? aLen : bLen;
  int c = memcmp(a, b, minLen);
  if (c != 0) return c;
  return aLen - bLen;
}

static uint8_t nextLeaf(const uint8_t* oid, int oidLen) {
  uint8_t candidate[MAX_BASE_OID_LEN + 1];
  memcpy(candidate, sBaseOid, sBaseOidLen);
  for (uint8_t leaf = 1; leaf <= NUM_OIDS; ++leaf) {
    candidate[sBaseOidLen] = leaf;
    if (oidCmp(oid, oidLen, candidate, sBaseOidLen + 1) < 0) return leaf;
  }
  return 0;
}

static bool isSysDescrOid(const uint8_t* oid, int oidLen) {
  return oidLen == (int)sizeof(kSysDescrOid) &&
         memcmp(oid, kSysDescrOid, sizeof(kSysDescrOid)) == 0;
}

int buildSnmpResponse(const uint8_t* req, int reqLen,
                       uint8_t* rsp, int rspMax)
{
  int pos = 0;
  if (!req || !rsp || reqLen <= 0) return -1;
  int outerLen = berReadField(req, reqLen, &pos, 0x30);
  if (outerLen < 0 || outerLen != reqLen - pos) return -1;
  int vl = berReadField(req, reqLen, &pos, 0x02);
  if (vl != 1) return -1;
  uint8_t version = req[pos]; pos += vl;
  if (version > 1) return -1;

  int cl = berReadField(req, reqLen, &pos, 0x04); if (cl<0) return -1;
  if (cl!=6 || memcmp(req+pos,"public",6)!=0) { pos+=cl; return -1; }
  pos += cl;

  if (pos >= reqLen) return -1;
  uint8_t pduType = req[pos];
  if (pduType!=0xA0 && pduType!=0xA1) return -1;
  int pduLen = berReadField(req, reqLen, &pos, pduType);
  if (pduLen < 0 || pduLen != reqLen - pos) return -1;
  int rl = berReadField(req, reqLen, &pos, 0x02); if (rl<=0||rl>4) return -1;
  const uint8_t* reqIdBytes = req + pos;
  pos += rl;
  for (int i = 0; i < 2; ++i) {
    int n = berReadField(req, reqLen, &pos, 0x02);
    if (n <= 0 || n > 4) return -1;
    pos += n;
  }
  int vblLen = berReadField(req, reqLen, &pos, 0x30);
  if (vblLen < 0 || vblLen != reqLen - pos) return -1;
  int vblEnd = pos + vblLen;

  int vbPos = 0;
  bool tooBig = false;
  uint8_t errorStatus = 0;
  uint8_t errorIndex = 0;
  uint8_t bindingIndex = 0;

  while (pos < vblEnd) {
    ++bindingIndex;
    int vbl2 = berReadField(req, vblEnd, &pos, 0x30); if (vbl2<0) return -1;
    int vbEnd2 = pos + vbl2;
    int oidLen = berReadField(req, vbEnd2, &pos, 0x06);
    if (oidLen<=0||oidLen>30) return -1;
    const uint8_t* oidData = req + pos;
    pos += oidLen;
    if (berReadField(req, vbEnd2, &pos, 0x05) != 0 || pos != vbEnd2) return -1;

    const bool isGetNext = (pduType == 0xA1);
    uint8_t respLeaf = 0;
    bool respIsSysDescr = false;
    if (isGetNext) {
      /* Include sysDescr in a walk started at .1, then continue into the
         product-specific enterprise branch. */
      if (oidCmp(oidData, oidLen, kSysDescrOid, sizeof(kSysDescrOid)) < 0) {
        respIsSysDescr = true;
      } else {
        respLeaf = nextLeaf(oidData, oidLen);
      }
    } else if (isSysDescrOid(oidData, oidLen)) {
      respIsSysDescr = true;
    } else {
      respLeaf = matchLeaf(oidData, oidLen);
    }

    DBG("respLeaf="); DBGLN(respLeaf);

    uint8_t vbTmp[300]; int vbTmpLen = 0;

    if (!respIsSysDescr && respLeaf == 0) {
      /* SNMP requires an exception varbind to contain the requested OID.
         The old code used <base>.23 here, causing the misleading .23.23
         output seen from net-snmp. */
      int written = berWRawOid(vbTmp + vbTmpLen, oidData, oidLen);
      if (written < 0) return -1;
      vbTmpLen += written;
      vbTmp[vbTmpLen++] = (version==0) ? 0x05 : isGetNext ? 0x82 : 0x80;
      vbTmp[vbTmpLen++] = 0x00;
      if (version == 0 && !errorStatus) {
        errorStatus = 2;
        errorIndex = bindingIndex;
      }
    } else if (respIsSysDescr) {
      vbTmpLen += berWRawOid(vbTmp + vbTmpLen, kSysDescrOid, sizeof(kSysDescrOid));
      vbTmpLen += berWStr(vbTmp + vbTmpLen, "ESP8266 Local Monitoring Logger", 31);
    } else if (isStrLeaf(respLeaf)) {
      char sval[260] = {0};
      getStrVal(respLeaf, sval, sizeof(sval));
      vbTmpLen += berWOid(vbTmp+vbTmpLen, respLeaf);
      vbTmpLen += berWStr(vbTmp+vbTmpLen, sval, strlen(sval));
    } else {
      vbTmpLen += berWOid(vbTmp+vbTmpLen, respLeaf);
      vbTmpLen += berWUint(vbTmp+vbTmpLen, leafTag(respLeaf), getNumVal(respLeaf));
    }

    if (tooBig || vbPos + 4 + vbTmpLen > (int)sizeof(sVbBuf)) {
      tooBig = true;
      continue;
    }
    sVbBuf[vbPos++] = 0x30;
    vbPos += berWLen(sVbBuf+vbPos, vbTmpLen);
    memcpy(sVbBuf+vbPos, vbTmp, vbTmpLen);
    vbPos += vbTmpLen;
  }

  int pp = 0;
  if (tooBig) { vbPos = 0; errorStatus = 1; errorIndex = 0; }
  // Echo the signed request-id bytes exactly, including negative NMS IDs.
  sPduBuf[pp++]=0x02; sPduBuf[pp++]=(uint8_t)rl;
  memcpy(sPduBuf+pp, reqIdBytes, rl); pp += rl;
  sPduBuf[pp++]=0x02; sPduBuf[pp++]=0x01; sPduBuf[pp++]=errorStatus;
  sPduBuf[pp++]=0x02; sPduBuf[pp++]=0x01; sPduBuf[pp++]=errorIndex;
  sPduBuf[pp++]=0x30; pp+=berWLen(sPduBuf+pp,vbPos);
  memcpy(sPduBuf+pp, sVbBuf, vbPos); pp+=vbPos;

  int ip = 0;
  sInnerBuf[ip++]=0x02; sInnerBuf[ip++]=0x01; sInnerBuf[ip++]=version;
  sInnerBuf[ip++]=0x04; sInnerBuf[ip++]=0x06;
  memcpy(sInnerBuf+ip,"public",6); ip+=6;
  sInnerBuf[ip++]=0xA2;
  ip += berWLen(sInnerBuf+ip, pp);
  memcpy(sInnerBuf+ip, sPduBuf, pp); ip+=pp;

  int rp = 0;
  if (1+3+ip > rspMax) return -1;
  rsp[rp++]=0x30;
  rp += berWLen(rsp+rp, ip);
  memcpy(rsp+rp, sInnerBuf, ip); rp+=ip;

  return rp;
}

template<typename UdpType>
void handleSNMP(UdpType& udp)
{
  int pktSize = udp.parsePacket();
  if (pktSize <= 0) return;
  if (pktSize > RX_BUF_SZ) { udp.discardReceived(); gSnmpRejected++; return; }

  int rxLen = udp.read(sRxBuf, RX_BUF_SZ);
  if (rxLen <= 0) return;
  gSnmpRequests++;

  IPAddress remoteIp   = udp.remoteIP();
  uint16_t  remotePort = udp.remotePort();

  DBG("SNMP from "); DBG(remoteIp); DBG(":"); DBGLN(remotePort);

  /* Never wait for an RS-485 transaction in the UDP request path.  A missing
     inverter can take seconds to answer; that exceeded Net-SNMP's default
     one-second timeout and made a healthy UDP service look dead.  The loop
     refreshes this cache independently every INVERTER_POLL_MS. */

  int txLen = buildSnmpResponse(sRxBuf, rxLen, sTxBuf, TX_BUF_SZ);

  if (txLen > 0) {
    if (!udp.beginPacket(remoteIp, remotePort) ||
        udp.write(sTxBuf, txLen) != (size_t)txLen ||
        !udp.endPacket()) {
      queueSnmpUdpRestart();
      return;
    }
    DBG("SNMP response sent bytes="); DBGLN(txLen);

    #ifdef SNMP_HEX_DUMP_ENABLED
    Serial.print("TX > ");
    for (int i=0;i<txLen;i++) {
      if(sTxBuf[i]<0x10) Serial.print('0');
      Serial.print(sTxBuf[i],HEX); Serial.print(' ');
    }
    Serial.println();
    #endif
  } else {
    DBGLN("SNMP bad request dropped");
    gSnmpRejected++;
  }
}

void queueSnmpUdpRestart(void) {
  /* Restart outside the packet handler so the socket is not torn down while
     the lwIP callback is still unwinding parse/read. */
  if (!gSnmpUdpRestartAtMs) gSnmpUdpRestartAtMs = millis() + 50UL;
  DBGLN("SNMP UDP listener restart queued");
}

void serviceSnmpUdpRestart(void) {
  if (!gSnmpUdpRestartAtMs) return;
  if ((long)(millis() - gSnmpUdpRestartAtMs) < 0) return;
  gUdp.stop();
  delay(2);
  gSnmpUdpReady = gUdp.begin(SNMP_PORT) != 0;
  gSnmpUdpRestartAtMs = gSnmpUdpReady ? 0 : millis() + SNMP_UDP_RETRY_MS;
  DBGLN(gSnmpUdpReady ? "SNMP UDP listener restarted" : "SNMP UDP bind failed; retry queued");
}

void refreshInverterData(bool force)
{
  if (!force && gDataValid && (millis()-gLastReadMs) < DATA_CACHE_MS) return;

  InverterData candidate = {};
  bool readOk = MODBUS_READ_ALL_REGISTERS(&candidate);
  if (!readOk) {
    cooperativeDelay(MODBUS_RETRY_GAP_MS);
    readOk = MODBUS_READ_ALL_REGISTERS(&candidate);
  }

  gLastReadMs = millis();
  if (readOk) {
    gInv = candidate;
    gDataValid = true;
    gLastGoodReadMs = gLastReadMs;
    gConsecutiveReadFailures = 0;
    DBGLN("Inverter OK");
    return;
  }

  if (gConsecutiveReadFailures < 255) gConsecutiveReadFailures++;
  const bool recentGoodData = gDataValid && gLastGoodReadMs != 0 &&
    (unsigned long)(gLastReadMs - gLastGoodReadMs) < LAST_GOOD_RETENTION_MS;
  if (!recentGoodData) gDataValid = false;

  DBG("Inverter read failed, consecutive failures=");
  DBG(gConsecutiveReadFailures);
  DBGLN(recentGoodData ? ", serving last-good cache" : ", no current data");
}

/* Background polling is independent of the network interfaces.  SNMP keeps
   its original on-request behaviour and normally serves this fresh cache. */
void pollInverter(void) {
  if ((unsigned long)(millis() - gLastPollMs) < INVERTER_POLL_MS) return;
  gLastPollMs = millis();
  DBGLN("Automatic inverter poll");
  refreshInverterData(true);
}

void serviceNetworkDuringWait(void) {
  if (gEthernetReady && !gServicingNetwork) {
    gServicingNetwork = true;
    uint32_t now = millis();
    if (gLastNetworkServiceMs) {
      uint32_t gap = now - gLastNetworkServiceMs;
      if (gap > gMaxNetworkGapMs) gMaxNetworkGapMs = gap;
    }
    gLastNetworkServiceMs = now;
    Ethernet.poll();
    if (gSnmpUdpReady && !Ethernet.hardwareFault()) handleSNMP(gUdp);
    gServicingNetwork = false;
  }
  yield();
}

void cooperativeDelay(uint32_t durationMs) {
  uint32_t started = millis();
  do {
    serviceNetworkDuringWait();
    delay(1);
  } while ((uint32_t)(millis() - started) < durationMs);
}

/* JSON is offered for diagnostics and new clients; the legacy $...# packet is
   kept unchanged for RMS clients that implement the documented TCP format. */
static void sendInverterJson(Print& output) {
  char packet[260] = {0};
  buildFullDataString(packet, sizeof(packet));
  output.print(F("{\"ok\":")); output.print(gDataValid ? F("true") : F("false"));
  output.print(F(",\"mac\":\"")); output.print(gMacID);
  output.print(F("\",\"packet\":\"")); output.print(packet);
  output.print(F("\",\"data\":{"));
  if (gDataValid) {
    output.print(F("\"output_voltage\":")); output.print(gInv.Output_Voltage_Raw);
    output.print(F(",\"output_frequency\":")); output.print(gInv.Output_Frequency_Raw / 10.0f, 1);
    output.print(F(",\"output_current\":")); output.print(gInv.Output_Current_Raw / 10.0f, 1);
    output.print(F(",\"output_power\":")); output.print(gInv.Output_Power_Raw);
    output.print(F(",\"input_voltage\":")); output.print(gInv.Input_Voltage_Raw);
    output.print(F(",\"input_frequency\":")); output.print(gInv.Input_Frequency_Raw / 10.0f, 1);
    output.print(F(",\"battery_voltage\":")); output.print(gInv.Battery_Voltage_Raw / 10.0f, 1);
    output.print(F(",\"battery_charging_current\":")); output.print(gInv.Battery_Charging_Current_Raw / 10.0f, 1);
    output.print(F(",\"solar_voltage\":")); output.print(gInv.Solar_Voltage_Raw / 10.0f, 1);
    output.print(F(",\"solar_current\":")); output.print(gInv.Solar_Current_Raw / 10.0f, 1);
    output.print(F(",\"solar_power\":")); output.print(gInv.Solar_Power_Raw);
    output.print(F(",\"output_energy\":")); output.print((uint32_t)gInv.Output_Energy_kWh * 1000UL + gInv.Output_Energy_Rem);
    output.print(F(",\"solar_energy\":")); output.print((uint32_t)gInv.Solar_Energy_kWh * 1000UL + gInv.Solar_Energy_Rem);
    output.print(F(",\"flash_writes\":")); output.print(gInv.Flash_Writes);
    output.print(F(",\"uptime_minutes\":")); output.print((uint32_t)gInv.Uptime_Minutes_k * 1000UL + gInv.Uptime_Minutes_Rem);
    output.print(F(",\"temperature\":")); output.print(gInv.Temperature_Raw / 10.0f, 1);
    output.print(F(",\"inverter_status_fault\":")); output.print(gInv.Inverter_Status_Fault);
    output.print(F(",\"pfc_charger_status_fault\":")); output.print(gInv.PFC_Charger_Status_Fault);
    output.print(F(",\"mppt_charger_status_fault\":")); output.print(gInv.MPPT_Charger_Status_Fault);
    output.print(F(",\"mains_grid_status_fault\":")); output.print(gInv.Mains_Grid_Status_Fault);
    output.print(F(",\"battery_status_fault\":")); output.print(gInv.Battery_Status_Fault);
  }
  output.print(F("},\"device_uptime_seconds\":")); output.print(millis() / 1000UL);
  output.print(F(",\"free_heap_bytes\":")); output.print(ESP.getFreeHeap());
  output.print(F(",\"firmware\":\"")); output.print(FIRMWARE_BUILD_LABEL);
  output.print(F("\",\"reset_reason\":\"")); output.print(ESP.getResetReason());
  output.print(F("\",\"ethernet_ip\":\"")); output.print(gDeviceIP);
  output.print(F("\",\"ethernet_mask\":\"")); output.print(Ethernet.subnetMask());
  output.print(F("\",\"ethernet_gateway\":\"")); output.print(Ethernet.gatewayIP());
  output.print(F("\",\"network_recoveries\":")); output.print(gNetworkRecoveries);
  output.print(F(",\"snmp_requests\":")); output.print(gSnmpRequests);
  output.print(F(",\"snmp_rejected\":")); output.print(gSnmpRejected);
  output.print(F(",\"arp_address_conflicts\":")); output.print(Ethernet.addressConflicts());
  output.print(F(",\"udp_send_timeouts\":")); output.print(Ethernet.udpSendTimeouts());
  output.print(F(",\"address_announcements\":")); output.print(gAddressAnnouncements);
  output.print(F(",\"link_transitions\":")); output.print(gLinkTransitions);
  output.print(F(",\"enc_rx_overflows\":")); output.print(Ethernet.receiveOverflows());
  output.print(F(",\"enc_tx_failures\":")); output.print(Ethernet.transmitFailures());
  output.print(F(",\"last_network_fault\":")); output.print(gLastNetworkFault);
  output.print(F(",\"max_network_service_gap_ms\":")); output.print(gMaxNetworkGapMs);
  output.print(F(",\"rs485_consecutive_failures\":")); output.print(gConsecutiveReadFailures);
  output.print(F(",\"rs485_last_good_age_seconds\":"));
  if (gLastGoodReadMs) output.print((unsigned long)(millis() - gLastGoodReadMs) / 1000UL);
  else output.print(F("null"));
  output.print(F(",\"ethernet_link\":\""));
  EthernetLinkStatus link = gEthernetReady ? Ethernet.linkStatus() : Unknown;
  output.print(link == LinkON ? F("up") : link == LinkOFF ? F("down") : F("unknown"));
  output.print(F("\",\"snmp_udp_ready\":")); output.print(gSnmpUdpReady ? F("true") : F("false"));
  output.print('}');
}

static IPAddress settingIP(const uint8_t value[4]) {
  return IPAddress(value[0], value[1], value[2], value[3]);
}

static uint32_t ipv4Number(const uint8_t value[4]) {
  return (uint32_t)value[0] << 24 | (uint32_t)value[1] << 16 |
         (uint32_t)value[2] << 8 | value[3];
}

static bool validStaticNetwork(const DeviceSettings& settings) {
  uint32_t ip = ipv4Number(settings.ip);
  uint32_t mask = ipv4Number(settings.subnet);
  uint32_t gateway = ipv4Number(settings.gateway);
  uint32_t hosts = ~mask;
  if (!mask || hosts < 3 || (hosts & (hosts + 1)) != 0) return false;
  if (!settings.ip[0] || settings.ip[0] == 127 || settings.ip[0] >= 224 ||
      !(ip & hosts) || (ip & hosts) == hosts) return false;
  // A zero gateway is valid for an isolated LAN, never for a routed NMS.
  if (gateway && (gateway == ip || (gateway & mask) != (ip & mask) ||
                  !(gateway & hosts) || (gateway & hosts) == hosts)) return false;
  return true;
}

void loadSettings(void) {
  EEPROM.begin(SETTINGS_EEPROM_SIZE);
  EEPROM.get(0, gSettings);
  if (gSettings.magic == SETTINGS_MAGIC) {
    /* An interrupted write or firmware from an older layout can leave an
       unterminated/invalid AP password.  softAP() then fails silently and
       the hotspot never becomes visible. */
    gSettings.apPassword[sizeof(gSettings.apPassword) - 1] = '\0';
    size_t passwordLength = strnlen(gSettings.apPassword, sizeof(gSettings.apPassword));
    bool settingsChanged = false;
    if (passwordLength < 8 || passwordLength > 63 ||
        strcmp(gSettings.apPassword, DEFAULT_AP_PASSWORD) != 0) {
      strncpy(gSettings.apPassword, DEFAULT_AP_PASSWORD, sizeof(gSettings.apPassword) - 1);
      gSettings.apPassword[sizeof(gSettings.apPassword) - 1] = '\0';
      settingsChanged = true;
      DBGLN("Hotspot password set to Tangent123");
    }
    /* The field did not exist in prior firmware.  An erased/migrated value
       is normal; use a safe default without discarding network settings. */
    if (gSettings.modbusTimeoutMs < MODBUS_TIMEOUT_MIN_MS ||
        gSettings.modbusTimeoutMs > MODBUS_TIMEOUT_MAX_MS) {
      gSettings.modbusTimeoutMs = MODBUS_TIMEOUT_DEFAULT_MS;
      settingsChanged = true;
    }
    if (settingsChanged) {
      EEPROM.put(0, gSettings);
      EEPROM.commit();
    }
    return;
  }

  memset(&gSettings, 0, sizeof(gSettings));
  gSettings.magic = SETTINGS_MAGIC;
  strncpy(gSettings.apPassword, DEFAULT_AP_PASSWORD, sizeof(gSettings.apPassword) - 1);
  /* Multi-device commissioning: a fresh controller has no shared production
     Ethernet address. Use its unique setup hotspot to save a unique static IP
     (recommended) or select DHCP before connecting all controllers together. */
  gSettings.useStaticIP = false;
  gSettings.modbusTimeoutMs = MODBUS_TIMEOUT_DEFAULT_MS;
  EEPROM.put(0, gSettings);
  EEPROM.commit();
}

/* Every ESP8266 has a factory-programmed chip ID.  Build a locally
   administered, unicast Ethernet MAC from it, so each flashed device has a
   stable unique Ethernet identity without sharing the ESP Wi-Fi MAC. */
void makeUniqueEthernetMac(void) {
  uint32_t chipId = ESP.getChipId();
  gMac[0] = 0x02; // locally administered, unicast
  gMac[1] = 0xE8;
  gMac[2] = 0x26;
  gMac[3] = (chipId >> 16) & 0xFF;
  gMac[4] = (chipId >> 8) & 0xFF;
  gMac[5] = chipId & 0xFF;
}

bool startEthernet(void) {
  gLastEthernetAttemptMs = millis();

  if (gSettings.useStaticIP) {
    /* A direct cable and an unmanaged switch do not provide DHCP.  Initialise
       the controller exactly once with its saved static address.  The old
       probe-then-begin sequence reset the ENC receive ring twice. */
    Ethernet.begin(gMac, settingIP(gSettings.ip), settingIP(gSettings.dns),
                   settingIP(gSettings.gateway), settingIP(gSettings.subnet));
    delay(500);
    if (Ethernet.hardwareStatus() != EthernetENC28J60 || Ethernet.hardwareFault()) {
      gDeviceIP = IPAddress(0,0,0,0);
      DBGLN("ENC28J60 not detected: check CS/SPI wiring and 3.3V power");
      return false;
    }
    gDeviceIP = Ethernet.localIP();
    DBG("Ethernet static IP: "); DBGLN(gDeviceIP);
    return gDeviceIP != IPAddress(0,0,0,0);
  }

  /* DHCP mode remains available for router installations. A direct cable has
     no DHCP server, so configure a unique static IP from the setup hotspot. */
  if (Ethernet.begin(gMac, 5000UL, 1000UL) != 0) {
    delay(500);
    gDeviceIP = Ethernet.localIP();
    DBG("Ethernet DHCP IP: "); DBGLN(gDeviceIP);
    return gDeviceIP != IPAddress(0,0,0,0);
  }

  gDeviceIP = IPAddress(0,0,0,0);
  DBGLN("Ethernet not configured; use the Wi-Fi setup hotspot");
  return false;
}

#if 0
/* Superseded v13 raw-frame prototype retained only for source comparison.
   The active build uses UIPEthernet above. */
static uint16_t rawRead16(const uint8_t* p) {
  return ((uint16_t)p[0] << 8) | p[1];
}

static void rawWrite16(uint8_t* p, uint16_t value) {
  p[0] = value >> 8;
  p[1] = value & 0xFF;
}

static uint16_t rawChecksum(const uint8_t* data, uint16_t length) {
  uint32_t sum = 0;
  while (length > 1) {
    sum += ((uint16_t)data[0] << 8) | data[1];
    data += 2;
    length -= 2;
  }
  if (length) sum += (uint16_t)data[0] << 8;
  while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
  return (uint16_t)~sum;
}

static bool rawIpIsDevice(const uint8_t* ip) {
  return ip[0] == gDeviceIP[0] && ip[1] == gDeviceIP[1] &&
         ip[2] == gDeviceIP[2] && ip[3] == gDeviceIP[3];
}

static bool rawSendFrame(uint16_t length) {
  uint16_t sent = gRawEthernet.sendFrame(sRawEthTx, length);
  if (sent == length) {
    ++gRawTxFrames;
    return true;
  }
  DBGLN("RAW Ethernet TX failed");
  return false;
}

/* Minimal static-IP Ethernet service.  ARP, ICMP echo and UDP/161 are all
   handled directly, while the existing BER/SNMP implementation is reused. */
void handleRawEthernet(void) {
  uint16_t frameLen = gRawEthernet.readFrame(sRawEthRx, sizeof(sRawEthRx));
  if (frameLen < 14) return;
  ++gRawRxFrames;

  uint16_t etherType = rawRead16(sRawEthRx + 12);
  if (etherType == 0x0806 && frameLen >= 42) {      // ARP
    const uint8_t* arp = sRawEthRx + 14;
    if (rawRead16(arp) != 1 || rawRead16(arp + 2) != 0x0800 ||
        arp[4] != 6 || arp[5] != 4 || rawRead16(arp + 6) != 1 ||
        !rawIpIsDevice(arp + 24)) return;

    memcpy(sRawEthTx, arp + 8, 6);                 // requester MAC
    memcpy(sRawEthTx + 6, gMac, 6);
    rawWrite16(sRawEthTx + 12, 0x0806);
    uint8_t* reply = sRawEthTx + 14;
    rawWrite16(reply, 1);
    rawWrite16(reply + 2, 0x0800);
    reply[4] = 6; reply[5] = 4;
    rawWrite16(reply + 6, 2);                      // ARP reply
    memcpy(reply + 8, gMac, 6);
    for (uint8_t i=0; i<4; ++i) reply[14+i] = gDeviceIP[i];
    memcpy(reply + 18, arp + 8, 6);
    memcpy(reply + 24, arp + 14, 4);
    if (rawSendFrame(42)) {
      DBG("RAW ARP reply to ");
      DBG(arp[14]); DBG('.'); DBG(arp[15]); DBG('.');
      DBG(arp[16]); DBG('.'); DBGLN(arp[17]);
    }
    return;
  }

  if (etherType != 0x0800 || frameLen < 34) return; // IPv4
  const uint8_t* ip = sRawEthRx + 14;
  uint8_t ihl = (ip[0] & 0x0F) * 4;
  uint16_t ipTotal = rawRead16(ip + 2);
  if ((ip[0] >> 4) != 4 || ihl < 20 || ipTotal < ihl ||
      (uint32_t)14 + ipTotal > frameLen || !rawIpIsDevice(ip + 16)) return;

  if (ip[9] == 1 && ipTotal >= (uint16_t)ihl + 8) { // ICMP echo
    const uint8_t* icmp = ip + ihl;
    if (icmp[0] != 8 || 14U + ipTotal > sizeof(sRawEthTx)) return;
    memcpy(sRawEthTx, sRawEthRx, 14 + ipTotal);
    memcpy(sRawEthTx, sRawEthRx + 6, 6);
    memcpy(sRawEthTx + 6, gMac, 6);
    uint8_t* outIp = sRawEthTx + 14;
    memcpy(outIp + 16, ip + 12, 4);
    for (uint8_t i=0; i<4; ++i) outIp[12+i] = gDeviceIP[i];
    outIp[8] = 64;
    outIp[10] = outIp[11] = 0;
    rawWrite16(outIp + 10, rawChecksum(outIp, ihl));
    uint8_t* outIcmp = outIp + ihl;
    uint16_t icmpLen = ipTotal - ihl;
    outIcmp[0] = 0;
    outIcmp[2] = outIcmp[3] = 0;
    rawWrite16(outIcmp + 2, rawChecksum(outIcmp, icmpLen));
    rawSendFrame(14 + ipTotal);
    return;
  }

  if (ip[9] != 17 || ipTotal < (uint16_t)ihl + 8) return; // UDP
  const uint8_t* udp = ip + ihl;
  uint16_t udpLen = rawRead16(udp + 4);
  if (rawRead16(udp + 2) != SNMP_PORT || udpLen < 8 ||
      udpLen > ipTotal - ihl) return;

  int snmpLen = buildSnmpResponse(udp + 8, udpLen - 8, sTxBuf, TX_BUF_SZ);
  if (snmpLen <= 0) {
    DBGLN("RAW SNMP bad request dropped");
    return;
  }

  uint16_t outIpTotal = 20 + 8 + snmpLen;
  uint16_t outFrameLen = 14 + outIpTotal;
  if (outFrameLen > sizeof(sRawEthTx)) return;
  memcpy(sRawEthTx, sRawEthRx + 6, 6);
  memcpy(sRawEthTx + 6, gMac, 6);
  rawWrite16(sRawEthTx + 12, 0x0800);
  uint8_t* outIp = sRawEthTx + 14;
  memset(outIp, 0, 20 + 8);
  outIp[0] = 0x45;
  rawWrite16(outIp + 2, outIpTotal);
  rawWrite16(outIp + 4, gRawIpId++);
  rawWrite16(outIp + 6, 0x4000);                  // don't fragment
  outIp[8] = 64;
  outIp[9] = 17;
  for (uint8_t i=0; i<4; ++i) outIp[12+i] = gDeviceIP[i];
  memcpy(outIp + 16, ip + 12, 4);
  rawWrite16(outIp + 10, rawChecksum(outIp, 20));
  uint8_t* outUdp = outIp + 20;
  rawWrite16(outUdp, SNMP_PORT);
  rawWrite16(outUdp + 2, rawRead16(udp));
  rawWrite16(outUdp + 4, 8 + snmpLen);
  rawWrite16(outUdp + 6, 0);                      // IPv4 UDP checksum optional
  memcpy(outUdp + 8, sTxBuf, snmpLen);
  if (rawSendFrame(outFrameLen)) {
    DBG("RAW SNMP response bytes="); DBGLN(snmpLen);
  }
}
#endif

static String formValue(const String& body, const char* name) {
  String key = String(name) + "=";
  int start = body.indexOf(key);
  if (start < 0) return String();
  start += key.length();
  int end = body.indexOf('&', start);
  String value = body.substring(start, end < 0 ? body.length() : end);
  value.replace("+", " ");
  return value;
}

static bool formIP(const String& body, const char* name, uint8_t destination[4]) {
  IPAddress address;
  if (!address.fromString(formValue(body, name))) return false;
  for (int i = 0; i < 4; ++i) destination[i] = address[i];
  return true;
}

static bool webIP(const char* name, uint8_t destination[4]) {
  IPAddress address;
  if (!address.fromString(gWiFiWebServer.arg(name))) return false;
  for (int i = 0; i < 4; ++i) destination[i] = address[i];
  return true;
}

static bool parseModbusTimeoutSeconds(const String& text, uint16_t* timeoutMs) {
  float seconds = text.toFloat();
  uint32_t milliseconds = (uint32_t)(seconds * 1000.0f + 0.5f);
  if (milliseconds < MODBUS_TIMEOUT_MIN_MS || milliseconds > MODBUS_TIMEOUT_MAX_MS) return false;
  *timeoutMs = (uint16_t)milliseconds;
  return true;
}

/* The setup AP must be available even when Ethernet/DHCP is unplugged or
   unavailable.  It gives phone access while Ethernet continues on the LAN. */
void startWiFiSetupAP(void) {
  if (gWiFiAPStarted || gWiFiAPWindowExpired) return;
  gLastWiFiAPAttemptMs = millis();
  String macWithoutColons = gMacID;
  macWithoutColons.replace(":", "");
  gApSSID = "InverterSetup_" + macWithoutColons;
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP);
  delay(100);
  /* Keep the setup AP on a different subnet from the direct static Ethernet
     network (192.168.4.0/24). */
  if (!WiFi.softAPConfig(IPAddress(192,168,10,1), IPAddress(192,168,10,1), IPAddress(255,255,255,0))) {
    DBGLN("ERROR: Wi-Fi setup hotspot IP configuration failed");
    return;
  }
  if (!WiFi.softAP(gApSSID.c_str(), gSettings.apPassword, 6, false, 4)) {
    DBGLN("ERROR: Wi-Fi setup hotspot failed to start");
    return;
  }
  gDnsServer.start(53, "*", WiFi.softAPIP());
  gWiFiWebServer.on("/", HTTP_ANY, handleWiFiConfigPage);
  gWiFiWebServer.on("/save", HTTP_POST, handleWiFiSaveConfig);
  gWiFiWebServer.on("/api/inverter", HTTP_ANY, handleWiFiInverterApi);
  /* Captive-portal probes used by Android, iOS/macOS and Windows. */
  gWiFiWebServer.on("/generate_204", HTTP_ANY, redirectCaptivePortal);
  gWiFiWebServer.on("/gen_204", HTTP_ANY, redirectCaptivePortal);
  gWiFiWebServer.on("/hotspot-detect.html", HTTP_ANY, redirectCaptivePortal);
  gWiFiWebServer.on("/connecttest.txt", HTTP_ANY, redirectCaptivePortal);
  gWiFiWebServer.on("/ncsi.txt", HTTP_ANY, redirectCaptivePortal);
  gWiFiWebServer.on("/fwlink", HTTP_ANY, redirectCaptivePortal);
  gWiFiWebServer.onNotFound(redirectCaptivePortal);
  gWiFiWebServer.begin();
  gWiFiAPStarted = true;
  gWiFiAPStartedMs = millis();
  delay(100); // Give the ESP8266 Wi-Fi stack time to begin beaconing.
  DBG("Wi-Fi setup hotspot SSID: "); DBGLN(gApSSID);
  DBG("Wi-Fi setup password: "); DBGLN(gSettings.apPassword);
  DBG("Wi-Fi setup page: http://"); DBGLN(WiFi.softAPIP());
  DBGLN("Wi-Fi setup hotspot is available for 10 minutes after reset");
}

static void stopWiFiSetupAP(void) {
  gDnsServer.stop();
  gWiFiWebServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  gWiFiAPStarted = false;
  gWiFiAPWindowExpired = true;
  DBGLN("Wi-Fi setup hotspot closed; press Reset to open it again");
}

void redirectCaptivePortal(void) {
  String uri = gWiFiWebServer.uri();
  gWiFiWebServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");

  /*
    Respond differently depending on the probe path so mobile OSes open
    the captive-portal browser reliably:
      - Android probes: /generate_204 or /gen_204 -> return a 302 redirect
      - iOS/macOS hotspot detect: /hotspot-detect.html -> return a small HTML
      - Windows NCSI: /connecttest.txt or /ncsi.txt -> return expected text
      - Fallback: redirect to the root page
  */
  if (uri == "/generate_204" || uri == "/gen_204") {
    gWiFiWebServer.sendHeader("Location", "http://192.168.10.1/", true);
    gWiFiWebServer.send(302, "text/plain", "");
  } else if (uri == "/hotspot-detect.html") {
    const char* html = "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>Success</title></head><body>Success</body></html>";
    gWiFiWebServer.send(200, "text/html", html);
  } else if (uri == "/connecttest.txt") {
    gWiFiWebServer.send(200, "text/plain", "Microsoft NCSI");
  } else if (uri == "/ncsi.txt") {
    gWiFiWebServer.send(200, "text/plain", "Microsoft NCSI");
  } else if (uri == "/fwlink") {
    gWiFiWebServer.sendHeader("Location", "http://192.168.10.1/", true);
    gWiFiWebServer.send(302, "text/plain", "");
  } else {
    /* Default: serve a tiny HTML page that immediately navigates to the UI */
    const char* page = "<!doctype html><html><head><meta http-equiv='refresh' content='0;url=/'>"
                       "<meta name=viewport content='width=device-width,initial-scale=1'><title>Inverter setup</title></head>"
                       "<body><p>Opening setup page...</p><p><a href='/'>Open manually</a></p></body></html>";
    gWiFiWebServer.send(200, "text/html", page);
  }
}

void handleWiFiConfigPage(void) {
  String page = F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><meta name=theme-color content='#0b1220'><title>Inverter Console</title>"
                  "<style>*{box-sizing:border-box}body{margin:0;background:#08111f;color:#e6edf7;font:15px Arial,sans-serif}.shell{max-width:760px;margin:auto;padding:24px 16px 42px}.hero{padding:24px;border:1px solid #253552;border-radius:20px;background:linear-gradient(135deg,#13233d,#0e1728 55%,#123f4b)}.eyebrow{color:#62dfca;font-size:11px;font-weight:bold;letter-spacing:1.4px}.hero h1{margin:8px 0 7px;font-size:27px}.hero p{margin:0;color:#aebed5;line-height:1.5}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:16px 0}.card,form{border:1px solid #273750;border-radius:16px;background:#101b2d;padding:17px;box-shadow:0 12px 28px #0003}.label{display:block;color:#91a5c2;font-size:12px;margin-bottom:7px}.value{font-size:17px;font-weight:bold;word-break:break-all}.pill{display:inline-block;color:#65e7c8;background:#0c3d3b;border:1px solid #227d75;border-radius:99px;padding:5px 9px;font-size:12px;font-weight:bold}.section{margin:22px 0 10px;font-size:17px}.hint{color:#9caeca;line-height:1.5}.form-card{margin-top:16px}fieldset{border:1px solid #30445f;border-radius:12px;margin:0 0 14px;padding:14px}legend{padding:0 7px;color:#64dfcb;font-weight:bold}label{display:block;margin:11px 0 5px;color:#c2cee0}input{width:100%;border:1px solid #3a506f;border-radius:9px;background:#091321;color:#f2f6fd;padding:12px;font-size:16px}.check{width:auto;margin:0 8px 0 0;accent-color:#3bd3b9}button{border:0;border-radius:10px;padding:12px 16px;background:linear-gradient(135deg,#39d2b9,#2786d7);color:#06121d;font-size:15px;font-weight:bold;cursor:pointer}.secondary{background:#1d304a;color:#dbe8f8;margin-bottom:10px}pre{display:none;white-space:pre-wrap;word-break:break-word;margin:10px 0 0;padding:13px;border-radius:10px;background:#07101d;border:1px solid #263b58;color:#91e7da;font-size:12px}@media(max-width:480px){.grid{grid-template-columns:1fr}.hero h1{font-size:23px}}</style></head><body><main class=shell><section class=hero><div class=eyebrow>LOCAL MONITORING LOGGER</div><h1>Inverter setup console</h1><p>Securely configure network access and view a live inverter reading.</p></section><section class=grid><div class=card><span class=label>ETHERNET ADDRESS</span><span class=value>");
  page += F("<style>body{background:#f5f7fb;color:#17233a}.hero{background:linear-gradient(135deg,#fff,#edf5ff);border-color:#d9e3f0;box-shadow:0 12px 30px #35507012}.hero:before{content:'●';color:#18a878;margin-right:8px}.eyebrow{color:#147a66}.hero p,.hint{color:#60718a}.card,form{background:#fff;border-color:#e1e8f1;box-shadow:0 10px 25px #243b5310}.label{color:#6d7e95}.value{color:#182b48}.pill{color:#08775d;background:#e7faf3;border-color:#bcebdc}fieldset{border-color:#dfe7f0}legend{color:#137962}label{color:#33445c}input{background:#fff;color:#17233a;border-color:#c9d6e4}input:focus{outline:2px solid #87c5ff;border-color:#398fe5}.check{accent-color:#138b72}button{background:linear-gradient(135deg,#1479d0,#1559ac);color:#fff;box-shadow:0 7px 14px #1b67b52b}.secondary{background:#eef5fd;color:#175ea6;border:1px solid #cbdff3;box-shadow:none}pre{background:#f7faff;border-color:#dce8f5;color:#145c74}</style>");
  page += gDeviceIP.toString();
  page += F("</span></div><div class=card><span class=label>SETUP HOTSPOT</span><span class=value>");
  page += gApSSID;
  page += F("</span></div><div class=card><span class=label>FIRMWARE VERSION</span><span class=value>");
  page += F(FIRMWARE_BUILD_LABEL);
  page += F("</span></div></section><span class=pill>HOTSPOT ONLINE</span><h2 class=section>Live data</h2><p class=hint>Use this button to request a the inverter reading.</p><button class=secondary type=button onclick=readInverter()>Read inverter</button><pre id=data></pre>"
            "<script>function readInverter(){var e=document.getElementById('data');e.style.display='block';e.textContent='Reading inverter...';fetch('/api/inverter').then(function(r){return r.json()}).then(function(d){e.textContent=JSON.stringify(d,null,2)}).catch(function(x){e.textContent='Request failed: '+x})}</script>"
            "<form class=form-card method=post action=/save><h2 class=section>Network configuration</h2><fieldset><legend>Ethernet addressing</legend><label><input class=check type=checkbox name=static value=1 ");
  if (gSettings.useStaticIP) page += F("checked");
  page += F("> Use static IPv4 address</label><label>IP address</label><input name=ip placeholder='Enter unique device IP' value='");
  page += settingIP(gSettings.ip).toString();
  page += F("'><label>Gateway</label><input name=gateway value='");
  page += settingIP(gSettings.gateway).toString();
  page += F("'><label>Subnet mask</label><input name=subnet value='");
  page += settingIP(gSettings.subnet).toString();
  page += F("'><label>DNS server</label><input name=dns value='");
  page += settingIP(gSettings.dns).toString();
  page += F("'></fieldset><fieldset><legend>Inverter communication</legend><label> response timeout <span class=hint>(seconds, 0.8 to 1.5)</span></label><input name=modbus_timeout type=number min=0.8 max=1.5 step=0.1 value='");
  page += String(gSettings.modbusTimeoutMs / 1000.0f, 1);
  page += F("'></fieldset><fieldset><legend>Hotspot security</legend><label>New hotspot password <span class=hint>(8-63 characters)</span></label><input name=password type=password maxlength=63 placeholder='Leave blank to keep current password'></fieldset><button type=submit>Save configuration &amp; restart</button></form><footer style='text-align:center;color:#7185a4;font-size:11px;margin-top:24px'>Developed by Bijendra</footer></main></body></html>");
  gWiFiWebServer.send(200, "text/html", page);
}

/* The hotspot has a different web server from Ethernet, so it needs its own
   API route.  Every button press forces the same Modbus read used by SNMP. */
void handleWiFiInverterApi(void) {
  refreshInverterData(true);
  char packet[260] = {0};
  buildFullDataString(packet, sizeof(packet));
  String json = F("{\"ok\":");
  json += gDataValid ? F("true") : F("false");
  json += F(",\"packet\":\"");
  json += packet;
  json += F("\"}");
  gWiFiWebServer.send(200, "application/json", json);
}

void handleWiFiSaveConfig(void) {
  DeviceSettings candidate = gSettings;
  String password = gWiFiWebServer.arg("password");
  bool useStatic = gWiFiWebServer.hasArg("static");
  uint16_t modbusTimeoutMs = 0;
  if (password.length() > 0 && (password.length() < 8 || password.length() > 63)) {
    gWiFiWebServer.send(400, "text/plain", "Hotspot password must be 8-63 characters.");
    return;
  }
  if (useStatic && (!webIP("ip", candidate.ip) || !webIP("gateway", candidate.gateway) ||
                    !webIP("subnet", candidate.subnet) || !webIP("dns", candidate.dns) ||
                    !validStaticNetwork(candidate))) {
    gWiFiWebServer.send(400, "text/plain", "Enter valid IPv4 addresses for all static fields.");
    return;
  }
  if (!parseModbusTimeoutSeconds(gWiFiWebServer.arg("modbus_timeout"), &modbusTimeoutMs)) {
    gWiFiWebServer.send(400, "text/plain", "RS-485 timeout must be between 0.8 and 1.5 seconds.");
    return;
  }
  gSettings = candidate;
  gSettings.useStaticIP = useStatic;
  gSettings.modbusTimeoutMs = modbusTimeoutMs;
  if (password.length() > 0) {
    strncpy(gSettings.apPassword, password.c_str(), sizeof(gSettings.apPassword) - 1);
    gSettings.apPassword[sizeof(gSettings.apPassword) - 1] = '\0';
  }
  gSettings.magic = SETTINGS_MAGIC;
  EEPROM.put(0, gSettings);
  EEPROM.commit();
  gWiFiWebServer.send(200, "text/html", "<h2>Saved.</h2><p>Device is restarting now.</p>");
  delay(800);
  ESP.restart();
}

static void sendEthernetPage(EthernetClient& client, const char* message = NULL) {
  client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                 "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><meta name=theme-color content='#0b1220'><title>Inverter Console</title>"
                 "<style>*{box-sizing:border-box}body{margin:0;background:#08111f;color:#e6edf7;font:15px Arial,sans-serif}.shell{max-width:760px;margin:auto;padding:24px 16px 42px}.hero{padding:24px;border:1px solid #253552;border-radius:20px;background:linear-gradient(135deg,#13233d,#0e1728 55%,#123f4b)}.eyebrow{color:#62dfca;font-size:11px;font-weight:bold;letter-spacing:1.4px}.hero h1{margin:8px 0 7px;font-size:27px}.hero p{margin:0;color:#aebed5;line-height:1.5}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:16px 0}.card,form{border:1px solid #273750;border-radius:16px;background:#101b2d;padding:17px;box-shadow:0 12px 28px #0003}.label{display:block;color:#91a5c2;font-size:12px;margin-bottom:7px}.value{font-size:17px;font-weight:bold;word-break:break-all}.pill{display:inline-block;color:#65e7c8;background:#0c3d3b;border:1px solid #227d75;border-radius:99px;padding:5px 9px;font-size:12px;font-weight:bold}.section{margin:22px 0 10px;font-size:17px}.hint{color:#9caeca;line-height:1.5}.notice{margin:14px 0;padding:11px 13px;border-left:3px solid #64dfcb;border-radius:8px;background:#123442;color:#d6fff7}.form-card{margin-top:16px}fieldset{border:1px solid #30445f;border-radius:12px;margin:0 0 14px;padding:14px}legend{padding:0 7px;color:#64dfcb;font-weight:bold}label{display:block;margin:11px 0 5px;color:#c2cee0}input{width:100%;border:1px solid #3a506f;border-radius:9px;background:#091321;color:#f2f6fd;padding:12px;font-size:16px}.radio{width:auto;margin:0 8px 0 0;accent-color:#3bd3b9}button{border:0;border-radius:10px;padding:12px 16px;background:linear-gradient(135deg,#39d2b9,#2786d7);color:#06121d;font-size:15px;font-weight:bold;cursor:pointer}.secondary{background:#1d304a;color:#dbe8f8;margin-bottom:10px}pre{display:none;white-space:pre-wrap;word-break:break-word;margin:10px 0 0;padding:13px;border-radius:10px;background:#07101d;border:1px solid #263b58;color:#91e7da;font-size:12px}@media(max-width:480px){.grid{grid-template-columns:1fr}.hero h1{font-size:23px}}</style></head><body><main class=shell><section class=hero><div class=eyebrow>LOCAL MONITORING LOGGER</div><h1>Inverter monitoring console</h1><p>Professional network setup, live inverter telemetry and SNMP readiness.</p></section><section class=grid><div class=card><span class=label>ACTIVE ETHERNET IP</span><span class=value>"));
  client.print(F("<style>body{background:#f5f7fb;color:#17233a}.hero{background:linear-gradient(135deg,#fff,#edf5ff);border-color:#d9e3f0;box-shadow:0 12px 30px #35507012}.hero:before{content:'●';color:#18a878;margin-right:8px}.eyebrow{color:#147a66}.hero p,.hint{color:#60718a}.card,form{background:#fff;border-color:#e1e8f1;box-shadow:0 10px 25px #243b5310}.label{color:#6d7e95}.value{color:#182b48}.pill{color:#08775d;background:#e7faf3;border-color:#bcebdc}fieldset{border-color:#dfe7f0}legend{color:#137962}label{color:#33445c}input{background:#fff;color:#17233a;border-color:#c9d6e4}input:focus{outline:2px solid #87c5ff;border-color:#398fe5}.radio{accent-color:#138b72}button{background:linear-gradient(135deg,#1479d0,#1559ac);color:#fff;box-shadow:0 7px 14px #1b67b52b}.secondary{background:#eef5fd;color:#175ea6;border:1px solid #cbdff3;box-shadow:none}pre{background:#f7faff;border-color:#dce8f5;color:#145c74}.notice{background:#edf9f5;color:#12664f;border-left-color:#24aa83}</style>"));
  client.print(gDeviceIP);
  client.print(F("</span></div><div class=card><span class=label>NETWORK MODE</span><span class=value>"));
  client.print(gSettings.useStaticIP ? F("Static") : F("DHCP"));
  client.print(F("</span></div><div class=card><span class=label>SETUP HOTSPOT</span><span class=value>"));
  client.print(gApSSID);
  client.print(F("</span></div><div class=card><span class=label>FIRMWARE VERSION</span><span class=value>"));
  client.print(F(FIRMWARE_BUILD_LABEL));
  client.print(F("</span></div><div class=card><span class=label>SNMP SERVICE</span><span class=value>v1/v2c · UDP 161</span></div></section>"));
  if (message) { client.print(F("<div class=notice>")); client.print(message); client.print(F("</div>")); }
  client.print(F("<span class=pill>INVERTER DATA: "));
  client.print(gDataValid ? F("available") : F("waiting for RS-485"));
  client.print(F("</span><h2 class=section>Live data</h2><p class=hint>Request a fresh RS-485 inverter reading without changing the current network configuration.</p><button class=secondary type=button onclick=readInverter()>Read inverter</button><pre id=data></pre>"
                 "<script>function readInverter(){var e=document.getElementById('data');e.style.display='block';e.textContent='Reading inverter...';fetch('/api/inverter?refresh=1').then(function(r){return r.json()}).then(function(d){e.textContent=JSON.stringify(d,null,2)}).catch(function(x){e.textContent='Request failed: '+x})}</script>"
                 "<form class=form-card method=post action=/save><h2 class=section>Network configuration</h2><fieldset><legend>Ethernet addressing</legend>"
                 "<label><input class=radio type=radio name=mode value=dhcp "));
  if (!gSettings.useStaticIP) client.print(F("checked"));
  client.print(F("> Use DHCP (router assigns the IP)</label><label><input class=radio type=radio name=mode value=static "));
  if (gSettings.useStaticIP) client.print(F("checked"));
  client.print(F("> Use static IPv4 address</label><label>IP address</label><input name=ip placeholder='Enter unique device IP' value='"));
  client.print(settingIP(gSettings.ip));
  client.print(F("'><label>Gateway</label><input name=gateway value='")); client.print(settingIP(gSettings.gateway));
  client.print(F("'><label>Subnet mask</label><input name=subnet value='")); client.print(settingIP(gSettings.subnet));
  client.print(F("'><label>DNS server</label><input name=dns value='")); client.print(settingIP(gSettings.dns));
  client.print(F("'></fieldset><button type=submit>Save configuration &amp; restart</button></form><p class=hint>After saving a static IP, reconnect using the new address.</p><footer style='text-align:center;color:#7185a4;font-size:11px;margin-top:24px'>Developed by Bijendra</footer></main></body></html>"));
}

/* TCP 5555 protocol:
     {CMD:"fetch", IP:"Slave_IP"}       -> legacy $IMEI,...# packet
     {"CMD":"fetch", "format":"json"} -> JSON object
   The IP field is accepted for compatibility; the response is always sent to
   the connected client, which avoids trusting a spoofed address in a request. */
void handleRmsTcp(void) {
  EthernetClient client = gRmsTcpServer.available();
  if (!client) return;

  String request;
  unsigned long deadline = millis() + 500;
  while (client.connected() && (int32_t)(millis() - deadline) < 0 && request.length() < 384) {
    while (client.available() && request.length() < 384) {
      char c = (char)client.read();
      request += c;
      if (c == '\n' || c == '}') goto requestComplete;
    }
    serviceNetworkDuringWait();
    delay(1);
  }
requestComplete:
  String normalized = request;
  normalized.toLowerCase();
  if (normalized.indexOf("fetch") < 0) {
    client.print(F("{\"ok\":false,\"error\":\"expected CMD fetch\"}\n"));
    DBGLN("TCP 5555: invalid request");
  } else {
    /* A TCP fetch explicitly asks for a new inverter transaction. */
    refreshInverterData(true);
    bool wantsJson = normalized.indexOf("json") >= 0;
    DBG("TCP 5555 fetch, response="); DBGLN(wantsJson ? "JSON" : "packet");
    if (wantsJson) {
      sendInverterJson(client);
      client.print('\n');
    } else {
      char packet[260] = {0};
      buildFullDataString(packet, sizeof(packet));
      client.print(packet);
    }
  }
  delay(2);
  client.stop();
}

void handleEthernetWeb(void) {
  EthernetClient client = gEthernetWebServer.available();
  if (!client) return;
  String request;
  unsigned long deadline = millis() + 1500;
  while (client.connected() && (int32_t)(millis() - deadline) < 0 && request.length() < 1200) {
    while (client.available() && request.length() < 1200) {
      request += (char)client.read();
      if (request.endsWith("\r\n\r\n")) goto headersComplete;
    }
    serviceNetworkDuringWait();
    delay(1);
  }
headersComplete:
  if (request.startsWith("GET /api/inverter")) {
    /* refresh=1 is used by the browser button to send a new Modbus request
       on every click rather than returning the five-second cached reading. */
    refreshInverterData(request.indexOf("refresh=1") >= 0);
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"));
    sendInverterJson(client);
    delay(1);
    client.stop();
    return;
  }
  bool isSave = request.startsWith("POST /save ");
  size_t contentLength = 0;
  int lengthStart = request.indexOf("Content-Length:");
  if (lengthStart >= 0) contentLength = (size_t)request.substring(lengthStart + 15).toInt();
  if (contentLength > 1024) {
    client.print(F("HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n\r\n"));
    client.stop();
    return;
  }
  String body;
  while (isSave && client.connected() && body.length() < contentLength && (int32_t)(millis() - deadline) < 0) {
    while (client.available() && body.length() < contentLength) body += (char)client.read();
    serviceNetworkDuringWait();
    delay(1);
  }
  if (isSave && body.length() != contentLength) {
    client.print(F("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"));
    client.stop();
    return;
  }
  if (isSave) {
    DeviceSettings candidate = gSettings;
    bool useStatic = formValue(body, "mode") == "static";
    if (useStatic && (!formIP(body, "ip", candidate.ip) || !formIP(body, "gateway", candidate.gateway) ||
                      !formIP(body, "subnet", candidate.subnet) || !formIP(body, "dns", candidate.dns) ||
                      !validStaticNetwork(candidate))) {
      sendEthernetPage(client, "Enter valid IPv4 addresses for all static fields.");
    } else {
      gSettings = candidate;
      gSettings.useStaticIP = useStatic;
      gSettings.magic = SETTINGS_MAGIC;
      EEPROM.put(0, gSettings);
      EEPROM.commit();
      sendEthernetPage(client, "Saved. Device is restarting now.");
      delay(800);
      ESP.restart();
    }
  } else sendEthernetPage(client);
  delay(1);
  client.stop();
}

void setup(void)
{
  Serial.begin(9600);
  DBG("Firmware build: "); DBGLN(FIRMWARE_BUILD_LABEL);
  pinMode(RS485_DE_PIN, OUTPUT);
  rs485_receive_enable();

  loadSettings();
  if (!configureSnmpBaseOid(SNMP_BASE_OID)) {
    DBGLN("Invalid SNMP_BASE_OID; SNMP is disabled");
    return;
  }

  makeUniqueEthernetMac();
  DBG("MAC: ");
  #ifdef DEBUG_ENABLED
  for (int i=0;i<6;i++) {
    if(gMac[i]<0x10) Serial.print('0');
    Serial.print(gMac[i],HEX);
    if(i<5) Serial.print(':');
  }
  Serial.println();
  #endif

  char mb[18];
  sprintf(mb,"%02X:%02X:%02X:%02X:%02X:%02X",
          gMac[0],gMac[1],gMac[2],gMac[3],gMac[4],gMac[5]);
  gMacID = String(mb);

  /* Ethernet remains the SNMP/RMS network.  The setup hotspot is opened only
     during the commissioning window so an engineer can identify the device,
     set Ethernet IP details and test inverter data from the local webpage. */
  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);
  gWiFiAPWindowExpired = false;
  DBGLN("Device Wi-Fi setup hotspot opens for commissioning after reset");

  /* Make the GPIO15 chip-select wiring explicit so direct-cable and
     switch/router installations use the identical binary. */
  Ethernet.init(ETHERNET_CS_PIN);

  DBGLN("Ethernet DHCP/static startup follows shortly");
  gLastEthernetAttemptMs = millis() - ETHERNET_RETRY_MS + INITIAL_ETHERNET_DELAY_MS;
  /* Make the first background inverter read happen immediately after setup. */
  gLastPollMs = millis() - INVERTER_POLL_MS;
  DBG("SNMP v1/v2c walk base: "); DBGLN(SNMP_BASE_OID);
}

void loop(void)
{
  serviceNetworkDuringWait();
  /* Read continuously: this runs whether Ethernet is connected or not. */
  pollInverter();

  if (!gWiFiAPStarted && !gWiFiAPWindowExpired &&
      (millis() - gLastWiFiAPAttemptMs >= WIFI_AP_RETRY_MS)) {
    startWiFiSetupAP();
  }

  /* Start EthernetENC with v13 addressing/fallback logic. */
  if (!gEthernetReady &&
      (millis() - gLastEthernetAttemptMs >= ETHERNET_RETRY_MS)) {
    DBGLN("Ethernet unavailable, retrying...");
    gEthernetReady = startEthernet();
    if (gEthernetReady) {
      gLastNetworkServiceMs = millis();
      gUdp.stop();
      gSnmpUdpReady = gUdp.begin(SNMP_PORT) != 0;
      gSnmpUdpRestartAtMs = gSnmpUdpReady ? 0 : millis() + SNMP_UDP_RETRY_MS;
      gEthernetLinkDownSinceMs = 0;
      gLinkState = Unknown;
      gAddressAnnouncesLeft = 0;
      if (!gEthernetWebStarted) {
        gEthernetWebServer.begin();
        gRmsTcpServer.begin();
        gEthernetWebStarted = true;
      }
      DBGLN("SNMP v1/v2c listening UDP 161, community=public");
      DBGLN("RMS TCP listening port 5555 (CMD fetch)");
      DBG("Ethernet setup page: http://"); DBGLN(gDeviceIP);
      gLastTxFailures = Ethernet.transmitFailures();
      gLastUdpTimeouts = Ethernet.udpSendTimeouts();
      gNextActiveProbeMs = millis() + 60000UL;
      gActiveProbeFailures = 0;
    }
  }

  if (gEthernetReady) {
    if ((uint32_t)(millis() - gLastLinkCheckMs) >= 500UL) {
      gLastLinkCheckMs = millis();
      EthernetLinkStatus nextLink = Ethernet.linkStatus();
      if (nextLink != gLinkState) {
        ++gLinkTransitions;
        if (nextLink == LinkON) {
          // Relearn the next hop after moving from the laptop to a site switch.
          Ethernet.clearArpCache();
          gAddressAnnouncesLeft = 4;
          gNextAddressAnnounceMs = millis();
        } else {
          gAddressAnnouncesLeft = 0;
        }
      }
      gLinkState = nextLink;
    }
    EthernetLinkStatus link = gLinkState;
    if (link == LinkOFF) {
      if (!gEthernetLinkDownSinceMs) gEthernetLinkDownSinceMs = millis();
    } else {
      gEthernetLinkDownSinceMs = 0;
    }
    bool linkLost = gEthernetLinkDownSinceMs &&
      (unsigned long)(millis() - gEthernetLinkDownSinceMs) >= ETHERNET_LINK_DOWN_GRACE_MS;

    bool isStalled = false;
    if (gEthernetReady && link == LinkON && Ethernet.localIP() != IPAddress(0,0,0,0) && (int32_t)(millis() - gNextActiveProbeMs) >= 0) {
      gNextActiveProbeMs = millis() + 60000UL;
      uint32_t txFailuresDelta = Ethernet.transmitFailures() - gLastTxFailures;
      uint32_t udpTimeoutsDelta = Ethernet.udpSendTimeouts() - gLastUdpTimeouts;
      gLastTxFailures = Ethernet.transmitFailures();
      gLastUdpTimeouts = Ethernet.udpSendTimeouts();

      bool gatewayProbeSuccess = Ethernet.resolveGateway();

      if (txFailuresDelta > 0 || udpTimeoutsDelta > 0) {
        gActiveProbeFailures++;
        if (gActiveProbeFailures >= 3) isStalled = true;
      } else if (!gatewayProbeSuccess) {
        gNextActiveProbeMs = millis() + 5000UL;
      } else {
        gActiveProbeFailures = 0;
      }
    }

    if (Ethernet.localIP() == IPAddress(0,0,0,0) || linkLost || Ethernet.hardwareFault() || isStalled) {
      gLastNetworkFault = Ethernet.hardwareFault() ? Ethernet.hardwareFaultReason() : (linkLost ? 6 : (isStalled ? 8 : 7));
      DBGLN("Ethernet address/link lost, restarting network");
      gEthernetReady = false;
      gNetworkRecoveries++;
      gUdp.stop();
      gSnmpUdpReady = false;
      gSnmpUdpRestartAtMs = 0;
      if (gEthernetWebStarted) {
        gEthernetWebServer.end();
        gRmsTcpServer.end();
        gEthernetWebStarted = false;
      }
      gLastEthernetAttemptMs = millis() - ETHERNET_RETRY_MS + 1000UL;
    } else {
      if (!gSettings.useStaticIP) {
        Ethernet.maintain();
        IPAddress renewedIP = Ethernet.localIP();
        if (renewedIP != IPAddress(0,0,0,0) && renewedIP != gDeviceIP) {
          gDeviceIP = renewedIP;
          DBG("Ethernet IP renewed: "); DBGLN(gDeviceIP);
        }
      }
      if (gSnmpUdpReady) handleSNMP(gUdp);
      serviceSnmpUdpRestart();
      handleRmsTcp();
      handleEthernetWeb();
      if (gLinkState == LinkON && gAddressAnnouncesLeft &&
          (int32_t)(millis() - gNextAddressAnnounceMs) >= 0) {
        bool announced = Ethernet.announceAddress();
        if (announced) ++gAddressAnnouncements;
        bool gatewayRequested = Ethernet.resolveGateway();
        if (announced && gatewayRequested) {
          --gAddressAnnouncesLeft;
          // Repeat while a managed switch may still be entering forwarding.
          gNextAddressAnnounceMs = millis() + (gAddressAnnouncesLeft >= 2 ? 2000UL : 15000UL);
        } else {
          gNextAddressAnnounceMs = millis() + 1000UL;
        }
      }
    }
  }
  if (gWiFiAPStarted) {
    gDnsServer.processNextRequest();
    gWiFiWebServer.handleClient();
    if ((unsigned long)(millis() - gWiFiAPStartedMs) >= WIFI_AP_CONFIG_WINDOW_MS) {
      stopWiFiSetupAP();
    }
  }
  cooperativeDelay(1);
}

void rs485_transmit_enable(void) { digitalWrite(RS485_DE_PIN,HIGH); delay(1); }
void rs485_receive_enable(void)  { digitalWrite(RS485_DE_PIN,LOW);  delay(1); }
uint16_t crc_cal_value(uint8_t* d, uint8_t len)
{
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= *d++;
    for (int i=0;i<8;i++)
      crc = (crc&1) ? (crc>>1)^0xA001 : crc>>1;
  }
  return crc;
}

bool MODBUS_READ_ALL_REGISTERS(InverterData* data)
{
  uint8_t cmd[8] = {0x0A,0x04,0x0B,0xB8,0x00,0x18,0x00,0x00};
  uint8_t rx[53] = {0};
  uint16_t drained = 0;

#ifdef SIMULATE_INVERTER
  /* Simulation mode: populate `data` with plausible values so UI, SNMP
     and TCP interfaces can be tested without the physical inverter. */
  data->Output_Voltage_Raw = 230;
  data->Output_Frequency_Raw = 500; /* 50.0 Hz */
  data->Output_Current_Raw = 120;  /* 12.0 A */
  data->Output_Power_Raw = 2760;
  data->Input_Voltage_Raw = 230;
  data->Input_Frequency_Raw = 500;
  data->Battery_Voltage_Raw = 520; /* 52.0 V */
  data->Battery_Charging_Current_Raw = 0;
  data->Solar_Voltage_Raw = 0;
  data->Solar_Current_Raw = 0;
  data->Solar_Power_Raw = 0;
  data->Output_Energy_kWh = 123;
  data->Output_Energy_Rem = 456;
  data->Solar_Energy_kWh = 0;
  data->Solar_Energy_Rem = 0;
  data->Flash_Writes = 10;
  data->Uptime_Minutes_k = 1;
  data->Uptime_Minutes_Rem = 234;
  data->Temperature_Raw = 250; /* 25.0 C */
  data->Inverter_Status_Fault = 0;
  data->PFC_Charger_Status_Fault = 0;
  data->MPPT_Charger_Status_Fault = 0;
  data->Mains_Grid_Status_Fault = 0;
  data->Battery_Status_Fault = 0;
  return true;
#endif

  while (Serial.available() && drained < 256) {
    Serial.read();
    drained++;
    if ((drained & 31) == 0) serviceNetworkDuringWait();
  }
  if (Serial.available()) return false;

  uint16_t crc = crc_cal_value(cmd,6);
  cmd[6]=crc&0xFF; cmd[7]=(crc>>8)&0xFF;

  Serial.flush();
  rs485_transmit_enable();
  delay(1);
  for (int i=0;i<8;i++) Serial.write(cmd[i]);
  Serial.flush();
  rs485_receive_enable();

  uint32_t t0 = millis();
  unsigned int  cnt = 0;
  bool frameValid = false;
  while ((uint32_t)(millis()-t0) < gSettings.modbusTimeoutMs) {
    // Sliding frame window accepts a valid reply after UART echo/noise. The
    // deadline is absolute, so a continuous noisy input cannot hold the loop.
    unsigned int budget = 32;
    while (Serial.available() && budget--) {
      if (cnt == sizeof(rx)) {
        memmove(rx, rx + 1, sizeof(rx) - 1);
        --cnt;
      }
      rx[cnt++] = Serial.read();
      if (cnt == sizeof(rx) && rx[0] == 0x0A && rx[1] == 0x04 && rx[2] == 0x30 &&
          crc_cal_value(rx, 51) == ((uint16_t)rx[52] << 8 | rx[51])) {
        frameValid = true;
        break;
      }
    }
    serviceNetworkDuringWait();
    if (frameValid) break;
  }

  #ifdef DEBUG_ENABLED
  Serial.print("Rx bytes: "); Serial.println(cnt);
  Serial.print("Rx > ");
  for (unsigned int i=0;i<cnt;i++) {
    if(rx[i]<0x10) Serial.print('0');
    Serial.print(rx[i],HEX); Serial.print(' ');
  }
  Serial.println();
  #endif

  if (!frameValid) return false;

  for (unsigned int i=0; i<=cnt-53; i++) {
    if (rx[i]!=0x0A || rx[i+1]!=0x04 || rx[i+2]!=0x30) continue;
    uint16_t rxCrc   = (rx[i+52]<<8)|rx[i+51];
    uint16_t calcCrc = crc_cal_value(&rx[i],51);
    if (rxCrc != calcCrc) continue;

    int d = i+3;
    data->Output_Voltage_Raw           = ((uint16_t)rx[d   ]<<8)|rx[d+ 1];
    data->Output_Frequency_Raw         = ((uint16_t)rx[d+ 2]<<8)|rx[d+ 3];
    data->Output_Current_Raw           = ((uint16_t)rx[d+ 4]<<8)|rx[d+ 5];
    data->Output_Power_Raw             = ((uint16_t)rx[d+ 6]<<8)|rx[d+ 7];
    data->Input_Voltage_Raw            = ((uint16_t)rx[d+ 8]<<8)|rx[d+ 9];
    data->Input_Frequency_Raw          = ((uint16_t)rx[d+10]<<8)|rx[d+11];
    data->Battery_Voltage_Raw          = ((uint16_t)rx[d+12]<<8)|rx[d+13];
    data->Battery_Charging_Current_Raw = ((uint16_t)rx[d+14]<<8)|rx[d+15];
    data->Solar_Voltage_Raw            = ((uint16_t)rx[d+16]<<8)|rx[d+17];
    data->Solar_Current_Raw            = ((uint16_t)rx[d+18]<<8)|rx[d+19];
    data->Solar_Power_Raw              = ((uint16_t)rx[d+20]<<8)|rx[d+21];
    data->Output_Energy_kWh            = ((uint16_t)rx[d+22]<<8)|rx[d+23];
    data->Output_Energy_Rem            = ((uint16_t)rx[d+24]<<8)|rx[d+25];
    data->Solar_Energy_kWh             = ((uint16_t)rx[d+26]<<8)|rx[d+27];
    data->Solar_Energy_Rem             = ((uint16_t)rx[d+28]<<8)|rx[d+29];
    data->Flash_Writes                 = ((uint16_t)rx[d+30]<<8)|rx[d+31];
    data->Uptime_Minutes_k             = ((uint16_t)rx[d+32]<<8)|rx[d+33];
    data->Uptime_Minutes_Rem           = ((uint16_t)rx[d+34]<<8)|rx[d+35];
    data->Temperature_Raw              = ((uint16_t)rx[d+36]<<8)|rx[d+37];
    data->Inverter_Status_Fault        = ((uint16_t)rx[d+38]<<8)|rx[d+39];
    data->PFC_Charger_Status_Fault     = ((uint16_t)rx[d+40]<<8)|rx[d+41];
    data->MPPT_Charger_Status_Fault    = ((uint16_t)rx[d+42]<<8)|rx[d+43];
    data->Mains_Grid_Status_Fault      = ((uint16_t)rx[d+44]<<8)|rx[d+45];
    data->Battery_Status_Fault         = ((uint16_t)rx[d+46]<<8)|rx[d+47];
    return true;
  }
  return false;
}
