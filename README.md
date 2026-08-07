![LoRaHAM_Pi](https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_logo.png?raw=true)

# LoRaHAM_Daemon (english) - Device Driver

LoRaHAM_Daemon is Raspberry Pi software for the LoRaHAM_Pi hardware upgrade project and LoRaHAM modules for amateur radio operators, enabling high-power LoRa operation with long range on a single-board computer. The daemon is a device driver that allows users (without any hardware programming knowledge) to easily operate the system.

First code for the LoRaHAM Pi hardware | https://www.loraham.de/produkt/loraham-pi/


<img src="https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_P1_3.jpg" alt="LoRaHAM_Pi" width="300" height="auto"><img src="https://github.com/LoRaHAM/LoRaHAM_Ressources/blob/main/LoRaHAM_Cartridge_for_pi500.png" alt="LoRaHAM Cartridge" width="300" height="auto">

* Raspberry Pi 3/4/5
* Raspbian Image on RPi

# The current daemon lives in `loraham_daemon/`

**[`loraham_daemon/`](loraham_daemon/) is the one canonical daemon source tree.**
The old single-file daemons are archived under [`legacy/`](legacy/) and must not
be built or deployed; everything else at the repository root is companion client
tooling. Full documentation — supported hardware, CLI, CONF protocol, framed
DATA protocol, systemd deployment: **[`loraham_daemon/README.md`](loraham_daemon/README.md)**.

Key facts (details in the daemon README):

* `--radio 433` or `--radio 868` is **mandatory**; one process drives one band.
  Dual-band operation runs two processes (systemd units `loraham-daemon@433`
  and `loraham-daemon@868`, shipped in `loraham_daemon/systemd/`).
* Hardware presets via `--hw` (legacy dual-module SX1278+RFM95, Uputronics
  Pi Zero LoRa boards, Waveshare SX1262 LoRaWAN/GNSS HAT).
* Sockets per band in `/run/loraham` (root:`loraham` 2750, via tmpfiles.d —
  create the group first: `sudo groupadd --system loraham`; clients need
  membership): raw DATA `lora{433,868}.sock`, framed DATA
  `lora{433,868}f.sock`, CONF `loraconf{433,868}.sock`.

> The historical `curl ... | sh` install script from loraham.de installs the
> **archived legacy daemon** and must not be used for current deployments.

# Build prerequisites on the Raspberry Pi image:

    sudo apt update
    sudo apt install g++ make cmake build-essential -y
    sudo apt install liblgpio-dev -y
    sudo apt install libncurses5-dev libncursesw5-dev -y
    sudo apt install socat -y

    git clone https://github.com/LoRaHAM/LoRaHAM_Daemon ~/LoRaHAM

    git clone https://github.com/jgromes/RadioLib ~/RadioLib
    git -C ~/RadioLib checkout 13b7c7cf84b191006da20f82bdb386f2efc96334

RadioLib is used from its source checkout by the daemon build script (no
`make install` required). The checkout above pins the exact RadioLib commit
the test suite and CI run against (see `.github/ci/radiolib.lock`) — building
against an arbitrary newer RadioLib may compile but is untested.

# Configure your Raspberry Pi Hardware Interface:

    sudo raspi-config nonint set_config_var dtparam=spi on /boot/firmware/config.txt # Enable SPI
    
    # Ensure dtoverlay=spi0-0cs is set in /boot/firmware/config.txt without altering dtoverlay=vc4-kms-v3d or dtparam=uart0
    sudo sed -i -e '/^\s*#\?\s*dtoverlay\s*=\s*vc4-kms-v3d/! s/^\s*#\?\s*(dtoverlay|dtparam\s*=\s*uart0)\s*=.*/dtoverlay=spi0-0cs/' /boot/firmware/config.txt
    
    # Insert dtoverlay=spi0-0cs after dtparam=spi=on if not already present
    if ! sudo grep -q '^\s*dtoverlay=spi0-0cs' /boot/firmware/config.txt; then
        sudo sed -i '/^\s*dtparam=spi=on/a dtoverlay=spi0-0cs' /boot/firmware/config.txt
    fi

# Compile instruction

loraham Daemon (canonical tree):

    cd ~/LoRaHAM/loraham_daemon
    ./build.sh            # release build (./build.sh --strict for -Werror)
    ./run_tests.sh        # full test suite

lorachat: 

    gcc lorachat_ncurses_113.c -o loraham_chat -lncurses -lpthread

loraham iGate:

    gcc -Wall -o loraham_igate loraham_iGate_105d.c

# Use instructions:
1. first run the LoRaHAM Daemon because this is the interface between hardware (LoRaHAM_Pi HAT or LoRaHAM Cartridge) and users programm
2. then the LoRaHAM iGate (OVERWATCH) or PiGate
3. or you can run the LoRaHAM Chat (but not with iGate)


yes you can run both programms, but the iGate will set Frequency at every TX and RX and your Chat will do the same.
But Chat use the RX-Frequency from iGate to transmitt and the TX-Frequency from the iGate to receive.
That will collide.
You can read on the Chat all incoming RF tranmissions to your iGate.

For a direct (non-systemd) run, point the daemon's sockets at /tmp — the
clients find them there automatically via their built-in fallback:

1. LORAHAM_SOCKET_DIR=/tmp ./loraham_daemon --radio 433   (and/or a second process: --radio 868)
2. ./loraham_igate
3. ./loraham_chat

(Under the systemd deployment the sockets live in /run/loraham instead and
clients pick them up there — see loraham_daemon/README.md.)

Daemon and iGate can also run as real daemon (parameter -d):
1. LORAHAM_SOCKET_DIR=/tmp ./loraham_daemon --radio 433 -d
2. ./loraham_igate -d

if you dont run loraham_daemon as a daemon, you see all traffic on your terminal!

iGate options:

    -c CALL      Rufzeichen
    -t TX_FREQ   TX Frequenz
    -r RX_FREQ   RX Frequenz
    -i SEK       Intervall IS
    -f SEK       Intervall RF
    -L LAT       Beacon Lat
    -O LON       Beacon Lon
    -R km        filter only pass stations arround xy kilometers
    -x LAT       filter from LAT
    -y LON       filter from LON (you can also set a station arround xy kilometers from other location different from yours)
    -S Symbold   Symbol of your map icons device
    -d           Run daemon in background

Example:

     ./loraham_igate -c DB0ABC-10 -t 433.900 -r 433.775 -L 4827.70N -O 00957.60E -f 600 -i 1200 -d
 
# Background information
loraham_daemon opens its IPC (inter process communication) UNIX sockets per band:

    - DATA433_SOCKET  "/run/loraham/lora433.sock"     (raw DATA)
    - DATA868_SOCKET  "/run/loraham/lora868.sock"
    - DATA433_FRAMED  "/run/loraham/lora433f.sock"    (framed DATA with RX metadata / TX_RESULT)
    - DATA868_FRAMED  "/run/loraham/lora868f.sock"
    - CONF433_SOCKET  "/run/loraham/loraconf433.sock" (CONF commands)
    - CONF868_SOCKET  "/run/loraham/loraconf868.sock"

The complete CONF command set (GET STATUS / GET STATS / GET CHANNEL, TX modes,
CAD policy, value ranges) is documented in loraham_daemon/README.md.
    
On the config sockets, you can send a simple text string to configurate the LoRa-Module from your programm:

    - "SET FREQ=433.900 SF=12 BW=125 CR=5 CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=1 POWER=17"
    - "SET FREQ=869.525 SF=11 BW=250 CR=5 CRC=1 PREAMBLE=16 SYNC=0x2B LDRO=1 POWER=10"

You can send this also from your terminal via socat:

    - echo "SET FREQ=433.900 SF=12 BW=125 CR=5 CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=1 POWER=17" | socat - UNIX-CONNECT:/run/loraham/loraconf433.sock
    - echo "SET FREQ=869.525 SF=11 BW=250 CR=5 CRC=1 PREAMBLE=16 SYNC=0x2B LDRO=1 POWER=10" | socat - UNIX-CONNECT:/run/loraham/loraconf868.sock
 
 thats the config for 433 LoRa-APRS and 868 Meshtastic in Ulm/Dornstadt Germany ;-)


# Warnings
This code is provided at your own risk and responsibility. This code is experimental.
For radio amateur or laboratory use only.

# Credits and license

    Copyright (c) 2020-2026 Alexander Walter
    Licensed under the GNU GPL v3 (see LICENSE)
    Maintained by Alexander Walter 
    
This project is licensed under the **GNU General Public License v3** — see [`LICENSE`](LICENSE)
for the full text. No further conditions apply: use, modification and redistribution, commercial
or not, are governed solely by the GPLv3.

    Copyright (C) 2026  LoRaHAM / Alexander Walter

    This program is free software: you can redistribute it and/or modify it under the terms
    of the GNU General Public License as published by the Free Software Foundation, either
    version 3 of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
    without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along with this program.
    If not, see <https://www.gnu.org/licenses/>.


![LoRaHAM_Pi](https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_logo.png?raw=true)

# LoRaHAM_Daemon (deutsch) - Gerätetreiber

LoRaHAM_Daemon ist eine Raspberry Pi Software für das Hardware-Upgrade-Projekt LoRaHAM_Pi und LoRaHAM Cartridge für Funkamateure, um LoRa mit hoher Sendeleistung und somit großer Reichweite auf einem Einplatinencomputer zu ermöglichen. Der Daemon ist ein Gerätetreiber, mit dem ein Nutzer (ohne Programmierkenntnisse der Hardware) diese einfach nutzen kann.

Erster Code für die LoRaHAM Pi Hardware | https://www.loraham.de/produkt/loraham-pi/

<img src="https://github.com/LoRaHAM/LoRaHAM_Pi/blob/main/LoRaHAM_P1_3.jpg" alt="LoRaHAM_Pi" width="300" height="auto"><img src="https://github.com/LoRaHAM/LoRaHAM_Ressources/blob/main/LoRaHAM_Cartridge_for_pi500.png" alt="LoRaHAM Cartridge" width="300" height="auto">

* Raspberry Pi 3/4/5
* Raspbian Image auf RPi
* 
# Der aktuelle Daemon liegt in `loraham_daemon/`

**[`loraham_daemon/`](loraham_daemon/) ist der einzige maßgebliche Daemon-Quellbaum.**
Die alten Einzeldatei-Daemons sind unter [`legacy/`](legacy/) archiviert und
dürfen nicht mehr gebaut oder eingesetzt werden. Vollständige Dokumentation
(Hardware, CLI, CONF-Protokoll, framed DATA, systemd-Betrieb):
**[`loraham_daemon/README.md`](loraham_daemon/README.md)**.

`--radio 433` oder `--radio 868` ist **Pflicht**; ein Prozess bedient ein Band.
Dualband-Betrieb läuft mit zwei Prozessen (`loraham-daemon@433` +
`loraham-daemon@868`). Das historische Installationsskript von loraham.de
installiert die archivierte Legacy-Version — nicht mehr verwenden.

Benötigte Pakete und RadioLib: siehe englischer Abschnitt oben (RadioLib wird
aus dem Quell-Checkout verwendet, kein `make install` nötig).

# Konfiguriere die SPI-Hardware des Raspberry Pi:

    sudo raspi-config nonint set_config_var dtparam=spi on /boot/firmware/config.txt # Enable SPI
    
    # Ensure dtoverlay=spi0-0cs is set in /boot/firmware/config.txt without altering dtoverlay=vc4-kms-v3d or dtparam=uart0
    sudo sed -i -e '/^\s*#\?\s*dtoverlay\s*=\s*vc4-kms-v3d/! s/^\s*#\?\s*(dtoverlay|dtparam\s*=\s*uart0)\s*=.*/dtoverlay=spi0-0cs/' /boot/firmware/config.txt
    
    # Insert dtoverlay=spi0-0cs after dtparam=spi=on if not already present
    if ! sudo grep -q '^\s*dtoverlay=spi0-0cs' /boot/firmware/config.txt; then
        sudo sed -i '/^\s*dtparam=spi=on/a dtoverlay=spi0-0cs' /boot/firmware/config.txt
    fi

# Kompilieranweisung

loraham Daemon (maßgeblicher Quellbaum):

    cd ~/LoRaHAM/loraham_daemon
    ./build.sh            # Release-Build (./build.sh --strict für -Werror)
    ./run_tests.sh        # komplette Testsuite

lorachat: 

    gcc lorachat_ncurses_113.c -o loraham_chat -lncurses -lpthread

loraham iGate:

    gcc -Wall -o loraham_igate loraham_iGate_105d.c

# Bedienungsanleitung:
1. Zuerst den LoRaHAM Daemon starten, da dies die Schnittstelle zwischen der Hardware (LoRaHAM_Pi HAT oder LoRaHAM Cartridge) und dem Benutzerprogramm ist.
2. Dann das LoRaHAM iGate (OVERWATCH) oder PiGate.
3. Oder Sie können den LoRaHAM Chat ausführen (aber nicht zusammen mit dem iGate).

Ja, Sie können beide Programme ausführen, aber das iGate wird die Frequenz bei jedem TX und RX setzen und Ihr Chat wird dasselbe tun.
Zudem nutzt der Chat die RX-Frequenz vom iGate zum Senden und die TX-Frequenz vom iGate zum Empfangen.
Das wird kollidieren.
Sie können im Chat alle eingehenden Funkübertragungen an Ihr iGate mitlesen.

Für einen direkten Start (ohne systemd) die Daemon-Sockets nach /tmp legen —
die Clients finden sie dort automatisch (eingebauter Fallback):

1. LORAHAM_SOCKET_DIR=/tmp ./loraham_daemon --radio 433   (und/oder zweiter Prozess: --radio 868)
2. ./loraham_igate
3. ./loraham_chat

Daemon und iGate können auch als echter Daemon laufen (Parameter -d):

1. LORAHAM_SOCKET_DIR=/tmp ./loraham_daemon --radio 433 -d
2. ./loraham_igate -d

Wenn Sie loraham_daemon nicht als Daemon ausführen, sehen Sie den gesamten Datenverkehr in Ihrem Terminal!

iGate Optionen:

    -c CALL      Rufzeichen
    -t TX_FREQ   TX Frequenz
    -r RX_FREQ   RX Frequenz
    -i SEK       Intervall IS
    -f SEK       Intervall RF
    -L LAT       Beacon Lat
    -O LON       Beacon Lon
    -R km        Filter: Nur Stationen im Umkreis von xy Kilometern durchlassen
    -x LAT       Filter von LAT
    -y LON       Filter von LON (Sie können auch eine Station im Umkreis von xy Kilometern von einem anderen Standort als Ihrem eigenen festlegen)
    -S Symbol    Symbol für Ihr Map-Icon Gerät
    -d           Daemon im Hintergrund ausführen

Beispiel:

     ./loraham_igate -c DB0ABC-10 -t 433.900 -r 433.775 -L 4827.70N -O 00957.60E -f 600 -i 1200 -d
 
# Hintergrundinformationen
loraham_daemon öffnet seine IPC (Inter-Process Communication) UNIX-Sockets pro Band:

    - DATA433_SOCKET  "/run/loraham/lora433.sock"     (rohes DATA)
    - DATA868_SOCKET  "/run/loraham/lora868.sock"
    - DATA433_FRAMED  "/run/loraham/lora433f.sock"    (framed DATA mit RX-Metadaten / TX_RESULT)
    - DATA868_FRAMED  "/run/loraham/lora868f.sock"
    - CONF433_SOCKET  "/run/loraham/loraconf433.sock" (CONF-Kommandos)
    - CONF868_SOCKET  "/run/loraham/loraconf868.sock"

Der vollständige CONF-Befehlssatz ist in loraham_daemon/README.md dokumentiert.
    
Über die Konfigurations-Sockets können Sie eine einfache Textzeichenfolge senden, um das LoRa-Modul aus Ihrem Programm heraus zu konfigurieren:

    - "SET FREQ=433.900 SF=12 BW=125 CR=5 CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=1 POWER=17"
    - "SET FREQ=869.525 SF=11 BW=250 CR=5 CRC=1 PREAMBLE=16 SYNC=0x2B LDRO=1 POWER=10"

Sie können dies auch von Ihrem Terminal über socat senden:

    - echo "SET FREQ=433.900 SF=12 BW=125 CR=5 CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=1 POWER=17" | socat - UNIX-CONNECT:/run/loraham/loraconf433.sock
    - echo "SET FREQ=869.525 SF=11 BW=250 CR=5 CRC=1 PREAMBLE=16 SYNC=0x2B LDRO=1 POWER=10" | socat - UNIX-CONNECT:/run/loraham/loraconf868.sock
 
Das ist die Konfiguration für 433 LoRa-APRS und 868 Meshtastic in Ulm/Dornstadt, Deutschland ;-)

# Warnungen
Dieser Code wird auf eigenes Risiko und eigene Verantwortung zur Verfügung gestellt. Dieser Code ist experimentell.
Er ist nur für Funkamateure oder Labore geeignet.

# Credits und Lizenz

    Copyright (c) 2020-2025 Alexander Walter
    Licensed under the GNU GPL v3 (see LICENSE)
    Maintained by Alexander Walter 
    
Dieses Projekt steht unter der **GNU General Public License v3** — der vollständige Text steht
in [`LICENSE`](LICENSE). Es gelten keine weiteren Bedingungen: Nutzung, Änderung und Weitergabe,
kommerziell oder nicht, richten sich ausschließlich nach der GPLv3.

    Copyright (C) 2026  LoRaHAM / Alexander Walter

    Dieses Programm ist freie Software: Sie können es unter den Bedingungen der GNU General
    Public License, wie von der Free Software Foundation veröffentlicht, weitergeben und/oder
    verändern; entweder gemäß Version 3 der Lizenz oder (nach Ihrer Wahl) jeder späteren
    Version.

    Die Veröffentlichung dieses Programms erfolgt in der Hoffnung, dass es Ihnen von Nutzen
    sein wird, aber OHNE IRGENDEINE GARANTIE, sogar ohne die implizite Garantie der MARKTREIFE
    oder der EIGNUNG FÜR EINEN BESTIMMTEN ZWECK. Details finden Sie in der GNU General Public
    License.

    Sie sollten ein Exemplar der GNU General Public License zusammen mit diesem Programm
    erhalten haben. Falls nicht, siehe <https://www.gnu.org/licenses/>.
