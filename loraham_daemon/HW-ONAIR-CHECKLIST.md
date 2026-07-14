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

## M4 — SX1262 (wird nach M4 ergänzt)

- Platzhalter: SX1262↔SX127x beide Richtungen (MeshCom- und LoRa-APRS-
  Parametersätze), Sync-Word-Kompatibilität 0x12/0x2B explizit, Long-Packet-
  TX/RX-Serie (Community-Hinweis: meshtasticd-Probleme mit langen Paketen auf
  diesem HAT reproduzieren oder ausschließen).
