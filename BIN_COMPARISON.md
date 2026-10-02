# Limited BIN Comparison

## Focus Areas & Findings

*   **Firmware Build Identity**:
    *   `ETPL_SNMP_NETWORK_20260930.bin` identity string: `SNMP_NETWORK_SERVICE_20260930`.
    *   `ETPL_SNMP_SITE_RECOVERY_20260930_R2.bin` identity string: `SNMP_SITE_RECOVERY_20260930_R2`.
    *   **Classification**: CONFIRMED FROM BINARY.

*   **ENC28J60 Diagnostics**:
    *   The `R2` binary added strings for `,"enc_tx_failures":` and `,"enc_rx_overflows":`.
    *   **Classification**: CONFIRMED FROM BINARY and SOURCE (`main.cpp` prints these to the JSON diagnostics output).

*   **Hardware Fault Handling & Link Monitoring**:
    *   The `R2` binary added strings for `,"last_network_fault":` and `,"link_transitions":`.
    *   **Classification**: CONFIRMED FROM BINARY and SOURCE.

*   **ARP/Address Announcements**:
    *   The `R2` binary added strings for `,"arp_address_conflicts":` and `,"address_announcements":`.
    *   **Classification**: CONFIRMED FROM BINARY and SOURCE.

*   **UDP Timeout Diagnostics**:
    *   The `R2` binary added the string `,"udp_send_timeouts":`.
    *   **Classification**: CONFIRMED FROM BINARY and SOURCE.

*   **Network Recovery Counters**:
    *   Both binaries have the `","network_recoveries":` counter. `R2` adds granularity through the new tracking features.
    *   **Classification**: CONFIRMED FROM BINARY.

*   **SNMP, TCP, Modbus, Wi-Fi AP**:
    *   No string-level diffs observed for these components. Their behavior/configuration logic remains constant across binaries.
    *   **Classification**: INFERRED (Behavioral parity assumed due to lack of distinct error string changes).

### Summary
The R2 baseline primarily introduced advanced diagnostics mapping driver-level states (`tx_failures`, `rx_overflows`, `last_network_fault`, `link_transitions`, `udp_send_timeouts`, `arp_address_conflicts`) up to the web JSON UI. These are not merely diagnostic but represent the actual state-tracking of failures underlying the existing recovery. This validates that any stall detection mechanism *must* use these `R2` additions instead of basic timers.
