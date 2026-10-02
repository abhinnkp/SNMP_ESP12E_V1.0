# Final Implementation Report: Production Firmware Hardening

## A. Modified files
- `src/main.cpp`
- `tests/test_network_recovery_native.py`

## B. Exact logic added
1. **Activity/Stall Tracking Variables**:
   Introduced variables to establish a cumulative baseline: `gLastTxFailures`, `gLastUdpTimeouts`, `gNextActiveProbeMs`, and `gActiveProbeFailures`.
2. **Re-Baselining**:
   Inside `startEthernet()`, when `gEthernetReady` is true, initialized the baselines based on the current state of `Ethernet.transmitFailures()` and `Ethernet.udpSendTimeouts()`.
3. **Stall Detection Loop**:
   Inside the `LinkON` check in `loop()`, added logic to periodically (every 60 seconds) verify the driver health via deltas:
   - Evaluated `txFailuresDelta = Ethernet.transmitFailures() - gLastTxFailures`
   - Evaluated `udpTimeoutsDelta = Ethernet.udpSendTimeouts() - gLastUdpTimeouts`
   - Requested a gateway ARP probe: `bool gatewayProbeSuccess = Ethernet.resolveGateway()`
   - If the active probe failed (or if the TX/UDP failure deltas increased), the counter `gActiveProbeFailures` increments. If it hits 3 consecutive fails, we flag `isStalled = true`.
4. **Soft Recovery execution**:
   Added `isStalled` to the existing recovery if-statement. Re-uses the *exact* existing recovery `gEthernetReady = false` flow without restarting the ESP. Logs a specific fault code `8` mapped back into `gLastNetworkFault` for accurate JSON diagnostics.

## C. EthernetENC semantics verified
- `transmitFailures()` and `udpSendTimeouts()` are purely cumulative across the lifetime of the ESP. Reinitializing SPI doesn't clear them, necessitating the `gLast...` deltas logic.
- `receiveOverflows()` triggers `EIR_RXERIF` gracefully into a `hardwareFault() == 5`, effectively recovering itself, so it doesn't need to be modeled directly in the stall detection.
- `resolveGateway()` executes a fast ARP packet. It fails securely (returning false) if the internal driver buffer (`in_packet != NOBLOCK`) is busy or the link is bad, making consecutive failures combined with actual metric faults highly accurate.

## D. Threshold rationale
- A single failure (ARP missed, TX collision) is ignored. The threshold is defined as **3 repeated failures on 60-second intervals**. This provides ~3 minutes of absolute silent lockup evidence or escalating TX failures before triggering a recovery, ensuring idle functionality operates flawlessly without unnecessary SPI soft resets.

## E. Existing functionality confirmed preserved
- Wi-Fi AP provisioning untouched.
- `platformio.ini` environment unchanged.
- Native regression strings passed (all prior tests pass, confirming no protocol behavior changed).

## F. Native test results
All 16 cases of `test_network_recovery_native.py` passed, successfully modeling timeouts, ARP responses, UDP tx failure conditions, and idle gaps properly.

## G. New health-logic test results
Added native test paths covering the 5 bounded states (idle, active, tx_fail, udp_fail, arp_fail). Validated the logic exclusively passes idle/active sequences, and correctly escalates failures to trigger the stall detection `(failed)` flag.

## H. Build result
SUCCESS (esp12e_nodemcu_115200)

## I. BIN filename
`ETPL_SNMP_SITE_RECOVERY_20260930_R3.bin`

## J. BIN size
353723 bytes

## K. SHA-256
`e5d5069fca51c3103d88674beb5587112e8ed1ca958df5d634103895cc3290b9`

## L. Any unresolved limitations
- Because no hardware ENC28J60 reset pin exists, if the chip experiences a complete hardware latch-up unresponsive to SPI instructions (`CS` toggle), the soft reset logic may not clear it (as noted by `faultReason = 1`). Hardware testing will clarify if SPI resets suffice.

## M. Hardware tests still required
1. Normal continuous operation.
2. Long-duration soak test to verify idle networks do not randomly reboot.
3. Switch port disable/enable (simulating link transition).
4. Physical network stall (disconnecting gateway router without dropping the local switch link).
5. Simultaneous Modbus + SNMP/TCP activity stress test to verify the bounded 60-sec probe doesn't choke normal packets.
