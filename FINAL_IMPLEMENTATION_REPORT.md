# Final Implementation Report: Production Firmware Hardening (R3)

## A. Corrected Implementation Summary
The stall detection logic has been reworked strictly per instructions. `resolveGateway()` is treated solely as an active ARP probe, **not** as conclusive proof of failure if it returns `false`. If the driver is busy constructing a packet and returns `false`, we simply retry sooner (`5000UL`) without penalizing the device. `gActiveProbeFailures` only increments when `txFailuresDelta > 0` or `udpTimeoutsDelta > 0`, confirming actual underlying transmission errors across the delta check. If the counters remain stable, the health check explicitly resets the failure count (`gActiveProbeFailures = 0`), meaning a truly idle network correctly stays running forever without unnecessary recoveries. The recovery logic falls through cleanly into the pre-existing block without rebooting the ESP.

## B. Exact Source Changes
**FILE:** `src/main.cpp`
```cpp
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
```

**FILE:** `tests/test_network_recovery_native.py`
Updated `stall_logic_*` checks to mirror C logic explicitly, handling the busy driver edge case correctly. Added stateful multi-cycle tests (`stall_logic_persistent_tx_fail` and `stall_logic_transient_tx_fail`) ensuring exactly 3 consecutive fails are required to trigger an actual stall reset.

## C. Test Results
- `test_stall_logic_idle` ... ok
- `test_stall_logic_active` ... ok
- `test_stall_logic_arp_fail` ... ok
- `test_stall_logic_tx_fail` ... ok
- `test_stall_logic_udp_fail` ... ok
- `test_stall_logic_persistent_tx_fail` ... ok
- `test_stall_logic_transient_tx_fail` ... ok
- ALL 21 ARP & packet test variants ... ok
- PlatformIO Compilation ... SUCCESS

## D. Hardware Validation Status
PENDING. Required real-world testing:
1. Normal long-duration operation / idle soak.
2. Switch port disable/enable.
3. Gateway unavailable while physical link remains UP.
4. Repeated network interruption/recovery.
5. Concurrent Modbus + SNMP + TCP traffic.

## E. Exact BIN filename
`ETPL_SNMP_SITE_RECOVERY_20260930_R3.bin`

## F. BIN size
353707 bytes

## G. SHA-256
`cbbe51f3876a0951d71794974018362e3a9d87ac24e89d926cf49e226e757f2c`

## H. Unresolved limitations
- **No hardware reset pin:** Because no hardware ENC28J60 reset pin exists, if the chip experiences a complete hardware latch-up unresponsive to SPI instructions (`CS` toggle), the soft reset logic may not clear it (as noted by `faultReason = 1`). Hardware testing will clarify if SPI resets suffice.

## I. Original Silent LINK-UP Failure Detection
**PARTIALLY DETECTED (LIMITATION NOTED).**
If the switch *silently* drops packets (MAC aging timeout) but the ENC28J60 successfully transmits the electrical signals without collision (`txFailures` does not increment), the device *cannot* conclusively detect the failure using TX failure counters alone. `resolveGateway()` will successfully enqueue the ARP packet, and `txFailuresDelta` will be `0`. Since there's no end-to-end response validation in this low-level loop (as EthernetENC is layer 2/3), the system will incorrectly think the network is healthy. This represents a fundamental limitation of relying strictly on MAC-layer failure counters when dealing with silent upstream switch issues.
