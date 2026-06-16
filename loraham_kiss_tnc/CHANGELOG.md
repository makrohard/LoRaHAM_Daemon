# 0.3.0
- Require loraham_daemon 110 framed RX metadata layout
- Strip daemon RX RSSI/SNR metadata before KISS output
- Add functional tests for daemon 110 framed RX packets

# 0.2.0
- Updated, after refactoring loraham_daemon to 109a. *This breaks comatibility to loraham_daemon < 109*
- Use framed DATA sockets
- TX Queue and delayed sending on TX=1 or CAD=1
- Use persistent CONF socket

# 0.1.0
- Initial Version, developed for loraham_daemon 108
- KISS/TCP server for APRS clients
- AX.25 / TNC2 / KISS frame handling
- LoRaHAM daemon socket integration
- CLI and config file support
- example configuration
- build and test helper scripts
- serial KISS forwarding  `start-serial-kiss.sh`
- README with usage notes
