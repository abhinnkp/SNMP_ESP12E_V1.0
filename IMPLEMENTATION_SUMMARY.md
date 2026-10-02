# Implementation Summary Preparation

## Implementation Proposal: Active Stalled Network Detection

The goal is to detect a state where the Physical Link is UP, and there's no `hardwareFault()`, but active outgoing network communication repeatedly fails or stalls (e.g. switch MAC timeout, silent TX engine lockup). We will rely on existing R2 metrics (`txFailures`, `udpSendTimeouts`) combined with an active probe `resolveGateway()`.

**Constraint Checklist & Confidence Score:**
1. No generic idle timeout? YES.
2. `resolveGateway()` not used alone? YES (combined with checking if TX failures are actively rising).
3. Modbus/Wi-Fi AP untouched? YES.
4. Uses existing recovery mechanism (`gEthernetReady = false`) without rebooting? YES.
5. In `main.cpp` using existing `Ethernet` APIs? YES.

### Proposed Code Changes

**FILE:** `src/main.cpp`

**FUNCTION:** `loop()`

**CURRENT BEHAVIOR:**
The loop currently checks for `linkLost` (LinkOFF for 30s) or `Ethernet.hardwareFault()`. If either is true, it sets `gEthernetReady = false`, stops the UDP socket, increments `gNetworkRecoveries`, and schedules a retry. If there's no fault and Link is ON, it handles DHCP, announces addresses, and services sockets. It does not handle silent TX stalls where the link stays UP.

**CHANGE:**
1. Add variables to track baseline failure metrics:
```cpp
static uint32_t gLastTxFailures = 0;
static uint32_t gLastUdpTimeouts = 0;
static unsigned long gLastActiveProbeMs = 0;
static uint8_t gActiveProbeFailures = 0;
```
2. Inside `if (gEthernetReady)`, within the `LinkON` check, evaluate stall conditions.
If the device has been running but there's a significant period (e.g. 5 minutes) where it decides to verify health:
   - Check if `Ethernet.transmitFailures()` or `Ethernet.udpSendTimeouts()` have increased significantly since the last check.
   - If failures are rising, trigger `Ethernet.resolveGateway()`.
   - If `resolveGateway()` returns false (indicating a driver-level refusal to send due to lockup) OR if it succeeds but the underlying TX failures continue to rise on subsequent checks, increment `gActiveProbeFailures`.
   - If `gActiveProbeFailures` exceeds a threshold (e.g. 3 consecutive failed checks spaced by 15 seconds), trigger a stall.
   - Inject the stall state into the existing recovery condition:
```cpp
    bool isStalled = (gActiveProbeFailures >= 3);
    if (Ethernet.localIP() == IPAddress(0,0,0,0) || linkLost || Ethernet.hardwareFault() || isStalled) {
      gLastNetworkFault = Ethernet.hardwareFault() ? Ethernet.hardwareFaultReason() : (linkLost ? 6 : (isStalled ? 8 : 7));
      // ... existing recovery
      gActiveProbeFailures = 0;
```

**WHY:**
This explicitly targets the field issue where the device thinks it's healthy but cannot effectively communicate. By tracking the *change* in driver-level failure counters (TX failures and UDP timeouts) and coupling them with an active ARP gateway resolution probe, we distinguish a genuinely broken network stack from a healthy but idle one.

**RISK:**
Low. Uses only existing exposed variables and the pre-existing, tested soft-reset mechanism. It does not interfere with the tight `pollInverter()` loop.

**TEST:**
- **Regression:** `tests/test_network_recovery_native.py` and `arp_regression.c` to ensure no native failures.
- **New targeted test:** Verify that a "mock" state of rising TX failures + failed gateway resolutions eventually triggers the soft-reset path, but zero traffic without rising TX failures does not.
- **Hardware test:** Disconnect router while leaving switch Link UP (simulate dead gateway / ARP timeout) and verify recovery loop triggers.
