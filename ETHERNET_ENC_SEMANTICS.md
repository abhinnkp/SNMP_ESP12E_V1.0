# EthernetENC Counter Semantics

*   **`transmitFailures()`**:
    *   Maps to `Enc28J60Network::txFailures`.
    *   Incremented in `sendPacket()` if SPI completes but `eir & EIR_TXERIF` remains true after `TX_COLLISION_RETRY_COUNT` retries.
    *   **Is Cumulative**: Yes. It initializes to 0 globally and is not reset on `init()`.

*   **`receiveOverflows()`**:
    *   Maps to `Enc28J60Network::rxOverflows`.
    *   Incremented in `receivePacket()` if `readReg(EIR) & EIR_RXERIF` is set.
    *   **Is Cumulative**: Yes. Also not reset on `init()`.

*   **`udpSendTimeouts()`**:
    *   Maps to `UIPEthernetClass::udp_send_timeouts`.
    *   Incremented in `tick()` (called by `poll()`) if an actively tracked UDP connection (`data->send`) has been pending for over 1000ms.
    *   **Is Cumulative**: Yes. Not reset by `init()`.

*   **`resolveGateway()`**:
    *   Uses `uip_arp_request()` to format an ARP broadcast for `uip_draddr` (gateway IP), then returns `network_send()`.
    *   If `in_packet != NOBLOCK` (we are processing an incoming packet) or `packetstate & UIPETHERNET_SENDPACKET` (a TCP/UDP packet is already queued), it immediately returns `false` without sending.
    *   Therefore, `resolveGateway() == false` can legitimately happen under normal load if called at the exact moment another packet is being built/read. This verifies that **it must not be used as an immediate trigger**. We must only trust *repeated* failures or combine it with actual TX failure increments.

### Reinitialization Check
Since `txFailures` and `udp_send_timeouts` are simple `static uint32_t` initialized to 0 at program start, `Ethernet.begin()` -> `Enc28J60Network::init()` does NOT reset them to 0. They are cumulative for the uptime of the ESP.

**Conclusion**: The logic in `loop()` must store baselines (`gLastTxFailures = Ethernet.transmitFailures()`) when the network becomes healthy, and compare deltas (`Ethernet.transmitFailures() - gLastTxFailures`) to detect stalls correctly.
