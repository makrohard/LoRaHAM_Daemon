# Multi-Hardware: On-Air-Checkliste für die Hardware-Session (Maintainer)

Software-seitig grün heißt NICHT on-air-korrekt; diese Liste ist der manuelle
Abnahmetest am Funkgerät. Alle RF-Übungen nur mit legalen Frequenz-/Leistungs-/
Duty-Cycle-Einstellungen (≤ 20 dBm).

## M3 — RadioDriver-Extraktion (A/B gegen den Vor-Treiber-Stand `834841a`)

Ziel: `--radio 433` / `--radio 868` (Profil `legacy`) verhalten sich mit dem
RadioDriver byte-identisch zum Stand vor der Extraktion. A = `834841a`,
B = M3-HEAD; jede Zeile auf beiden Ständen ausführen und vergleichen.

| # | Test | Erwartung A == B |
|---|---|---|
| 1 | RX-Decode: bekannte Gegenstation (LoRa-APRS 433.900 SF12), 10 Pakete | identische Dekodierung, RSSI/SNR-Metadaten plausibel, `RX`-Zähler in `GET STATS` |
| 2 | TX raw DATA (beide Bänder), Gegenstation dekodiert | `TX=1/0`-Broadcasts, `TXOK`-Zähler, Inhalt korrekt empfangen |
| 3 | TX framed + `SET TXRESULT=1`: MANAGED (Queue) und DIRECT | `TX_RESULT` OK; MANAGED: flags Bit0 gesetzt; DIRECT: Bit0 leer |
| 4 | CAD-Timeout-Pfad: Kanal mit Dauerträger/Traffic belegen, MANAGED TX | `CHANNEL_BUSY` nach `CADWAIT` (Default 1500 ms); mit `SET CADTXAFTERTIMEOUT=1` Sendung nach Timeout + Flag Bit2 |
| 5 | CAD-Monitor: `SET CADMONITOR=1`, echten Traffic erzeugen | `CAD=1`-Flanke bei Aktivität, `CAD=0` erst nach Hysterese (2 Samples ≥3 dB unter `CADRSSI`) |
| 6 | `GET CHANNEL` in Ruhe und während fremdem Träger | `CADSCAN=1`, `CADSTATE=FREE/BUSY` korrekt; während eigenem TX: `BUSY=1 CADSTATE=UNAVAILABLE` |
| 7 | GETRSSI-Stream: Träger ein-/ausschalten | Pegelsprung sichtbar, Werteplateau plausibel (Offset −164/−157 LF/HF) |
| 8 | MODE-Wechsel LoRa→FSK→LoRa per CONF, danach RX/TX | Wechsel ohne Absturz, RX-Callback wieder aktiv, Parameter-Apply im jeweiligen Modus |
| 9 | `GET STATUS`/`GET STATS` nach Testlauf | Feldreihenfolge und Zählerstände identisch zu A |
| 10 | LED-Verhalten (Profil `legacy`): RX-Blitz, TX/CAD-aktiv | identisches Blinkbild |

## M4 — SX1262 (Waveshare LoRaWAN Node HAT, LF und HF)

Vorbereitung: `dtoverlay=spi0-0cs`, HAT aufgesteckt, Start mit
`--radio <band> --hw waveshare-sx1262`. Erwartung ohne HAT: `begin()` −2,
eine Diagnosezeile, Daemon beendet sich fail-closed (softwareseitig getestet).

| # | Test | Erwartung |
|---|---|---|
| 1 | Init mit HAT (beide Bandbindungen: LF-Variante 433, HF-Variante 868) | `Init OK`, `GET STATUS RADIO=READY`; TCXO-Spannung (DIO3) wirksam, kein `CHIP_NOT_FOUND` |
| 2 | Sync-Word-Kompatibilität explizit: SX1262 ↔ SX127x-Gegenstation, `SYNC 0x12` (LoRa-APRS) und `0x2B` (MeshCom-Raster) | beide Richtungen dekodieren; wenn nicht: RadioLib-Mapping (Steuerbits 0x44) als erste Verdächtige dokumentieren |
| 3 | SX1262→SX127x TX, beide Parametersätze (LoRa-APRS 433.900/SF12/BW125; MeshCom 433.175/SF11/BW250/CR6) | Gegenstation dekodiert; Frequenzablage/Drift im Rahmen (TCXO) |
| 4 | SX127x→SX1262 RX, gleiche Sätze | RX_PACKET mit plausiblen RSSI/SNR-Metadaten |
| 5 | Long-Packet-Serie: ≥50 Pakete à 200–255 Byte in beide Richtungen (§1.3-Hinweis: meshtasticd-Longpacket-Probleme auf diesem HAT) | 0 Verluste jenseits der Funkstrecke, keine Hänger; BUSY-Timeouts im Log = Befund |
| 6 | MANAGED TX + CAD: Kanal belegen, `CHANNEL_BUSY`-Pfad; `GET CHANNEL` | SX126x-scanChannel liefert BUSY/FREE korrekt, `CADSCAN=1` |
| 7 | GETRSSI-Stream + CAD-Monitor (`SET CADMONITOR=1`, `CADRSSI` variieren) | Pegel plausibel (GetRssiInst), Flanken wie beim SX127x |
| 8 | TX-Leistungsklemme: `SET POWER=0/10/20` | Übernahme grün; Ausgangsleistung messbar gestaffelt (Chip-Bereich −9…+22) |
| 9 | FSK: `MODE=FSK`, BR/FREQDEV/RXBW (SX126x-Raster!), SHAPING, ENCODING; `SET OOK=1` | Parametersatz wirksam; OOK deutlich abgelehnt; RXBW-Fremdwerte rot |
| 10 | TXEN/ANT_SW (BCM 6): TX-Ausgangsleistung mit/ohne korrekt gesetztem RF-Switch | volle Leistung nur mit `setDio2AsRfSwitch` + TXEN-Pfad (Regressionswache für die RF-Switch-Verdrahtung) |
| 11 | Kombination: Waveshare-Prozess + Uputronics-CE0-Prozess parallel | zweiter Prozess scheitert NICHT auf Funkpins; LED-Konflikt BCM 6 gemäß README-Matrix (CE0-LED-Reassign offen) |
