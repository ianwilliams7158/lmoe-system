# LMoE — Last Man on Earth

**Survival Intelligence System** - offline-first situational awareness for when the grid goes down.

LMoE is a self-contained browser-based application that runs locally on a Raspberry Pi or Windows PC. It combines maps, satellite tracking, weather, LoRa radio, a survival library, supply run planning, and a companion field terminal (Espy), made from a cheap ESP32 CYD, into a single resilient system designed to work with or without internet access.

---

## Quick Install

### Raspberry Pi / Linux

Open a terminal and run:

```bash
curl -sSL https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main/installer/install_linux.sh | bash
```

That single command downloads everything, installs dependencies, and launches the setup wizard. After setup completes, type `lmoe` in any terminal to open LMoE, or use the desktop shortcut.

### Windows

Open PowerShell and run:

```powershell
Set-ExecutionPolicy -Scope CurrentUser Bypass -Force
irm https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main/installer/install_windows.ps1 | iex
```

Or download `installer/install_windows.ps1`, right-click it, and select **Run with PowerShell**.

---

## What the Installer Does

1. Installs Python 3.9+ and required packages (`libzim`, `esptool`, `beautifulsoup4`)
2. Downloads all LMoE files to `~/lmoe` (Linux) or `C:\LMoE` (Windows)
3. Installs background services:
   - **lmoe-proxy** — local OpenSky proxy for authenticated flight tracking
   - **lmoe-sync** — Espy field terminal sync server
4. Creates a desktop shortcut and `lmoe` terminal command
5. Launches the **Setup Wizard** which configures:
   - Operator name and home location
   - API keys (AISStream for ships, OpenSky for flights)
   - ZIM offline library path and index
   - Espy firmware flash (optional)

Re-run setup at any time:
```bash
lmoe-setup          # Linux
# or
python3 ~/lmoe/setup_wizard.py
```

---

## Repository Structure

```
lmoe-system/
├── lmoe/
│   ├── lmoe.html              Main application (single HTML file, ~4MB)
│   ├── lmoe_config.json       Configuration template
│   ├── lmoe_proxy.py          OpenSky authenticated proxy server
│   ├── lmoe_sync_server.py    Espy field terminal sync server
│   └── lmoe_zim_indexer.py    ZIM file indexer for Intelligence panel
│
├── espy/
│   └── firmware/
│       ├── main.cpp           Espy firmware source (ESP32-2432S028R)
│       ├── platformio.ini     PlatformIO build config
│       └── lmoe_espy.bin      Pre-compiled firmware (flash with installer)
│
├── installer/
│   ├── install_linux.sh       Linux/Pi single-command installer
│   ├── install_windows.ps1    Windows PowerShell installer
│   └── setup_wizard.py        Cross-platform setup wizard
│
└── docs/
    └── api_keys.md            How to get each API key
```

---

## Features

| Feature | Online | Offline |
|---|---|---|
| Topographic & satellite maps | ✓ BKG / Esri tiles | ✓ PMTiles local files |
| Satellite tracking (ISS, weather sats) | ✓ live TLE refresh | ✓ embedded TLEs |
| Live ship positions (AIS) | ✓ AISStream WebSocket | — |
| Live flight positions | ✓ OpenSky Network | — |
| Weather & forecast | ✓ Open-Meteo API | — |
| Nuclear exclusion zones | ✓ built-in | ✓ built-in |
| Survival library (PDF, EPUB, DOCX) | ✓ | ✓ IndexedDB |
| ZIM offline knowledge base | ✓ | ✓ local files |
| Intelligence / RAG search | ✓ | ✓ |
| LoRa radio integration | ✓ | ✓ serial |
| Journal, resources, supply runs | ✓ | ✓ localStorage |
| Espy field terminal sync | ✓ WiFi | ✓ WiFi LAN |

---

## API Keys

All API keys are optional. Without them, features degrade gracefully to anonymous / limited mode.

See [docs/api_keys.md](docs/api_keys.md) for step-by-step instructions on getting each key.

| Service | Used for | Free tier | Key required? |
|---|---|---|---|
| [AISStream](https://aisstream.io) | Live ship positions | Unlimited | Recommended |
| [OpenSky](https://opensky-network.org) | Live flight positions | 400 req/day | Optional |
| [Open-Meteo](https://open-meteo.com) | Weather & forecast | Unlimited | No |
| [Nominatim](https://nominatim.org) | Location search | Free | No |
| [BKG TopPlusOpen](https://gdz.bkg.bund.de) | Topographic maps | Free | No |

---

## Espy Field Terminal

Espy is a companion device built on the **ESP32-2432S028R** (Cheap Yellow Display / CYD) with a 3D-printed case, internal battery, and SD card. It connects to LMoE via WiFi and is designed to go with you on supply runs when you cannot take the main screen.

The installer flashes Espy automatically if you plug it in during setup. To flash manually:

```bash
pip install esptool
esptool.py --port /dev/ttyUSB0 --baud 921600 write_flash 0x0000 ~/lmoe/espy/firmware/lmoe_espy.bin
```

To build from source (requires [PlatformIO](https://platformio.org)):

```bash
cd ~/lmoe/espy/firmware
pio run
# Binary at: .pio/build/esp32dev/firmware.bin
```

---

## Resources

- **Kiwix Desktop** (open ZIM files): https://www.kiwix.org/en/downloads/
- **ZIM files** (offline Wikipedia, survival guides): https://library.kiwix.org
- **Offline map tiles** (PMTiles): https://protomaps.com/downloads
- **PlatformIO** (Espy build only): https://platformio.org/install/cli

---

## Licence

Private — all rights reserved. Not for redistribution.
