# LMoE — API Keys Guide

All API keys are optional. LMoE works without them — features just run in limited or anonymous mode. This document explains exactly where to get each one and what you get from it.

---

## AISStream — Ship Watch

**What it does:** Provides a real-time WebSocket feed of AIS (Automatic Identification System) data from ships worldwide. Used by LMoE's Ship Watch panel to show live vessel positions, headings, speed, and vessel details on the map.

**Without a key:** Ship Watch still works but is limited to a small number of anonymous requests per day, and the connection may drop more frequently.

**Cost:** Free. No credit card required.

**Steps:**

1. Go to **https://aisstream.io**
2. Click **Sign Up** — use any email address
3. Confirm your email
4. Log in and go to **Dashboard → API Keys**
5. Click **Create API Key**
6. Copy the key (a long string of letters and numbers)
7. Paste it in the LMoE Setup Wizard, or in LMoE → Settings → Ship Watch

---

## OpenSky Network — Flight Watch

**What it does:** Provides real-time aircraft position data from a global network of ADS-B receivers. Used by LMoE's Flight Watch panel to show live aircraft on the map.

**Without credentials:** Flight Watch works anonymously at 100 API requests per day — enough for occasional checking but not continuous monitoring.

**With credentials:** 400 API requests per day, and the LMoE proxy server handles authentication locally so your credentials never leave your machine.

**Cost:** Free. No credit card required.

**Steps:**

1. Go to **https://opensky-network.org**
2. Click **Register** — fill in username, email, password
3. Confirm your email
4. Log in, click your username (top right) → **My OpenSky**
5. Go to the **API** tab
6. Click **Create new client** — give it a name (e.g. "LMoE")
7. Copy the **Client ID** and **Client Secret**
8. Paste both into the LMoE Setup Wizard, or in LMoE → Settings → Flight Watch

---

## Open-Meteo — Weather

**What it does:** Provides weather forecasts, current conditions, sunrise/sunset times, and historical weather data. Used by LMoE's Weather panel.

**No key required.** Open-Meteo is a free, open-source weather API with no authentication needed. LMoE uses it automatically.

Website: **https://open-meteo.com**

---

## Nominatim — Location Search

**What it does:** Converts place names to coordinates (geocoding). Used when you search for a location in LMoE Settings → Identity.

**No key required.** Nominatim is the OpenStreetMap geocoding service and is free to use with reasonable usage limits. LMoE uses it automatically.

Website: **https://nominatim.openstreetmap.org**

---

## BKG TopPlusOpen — Maps

**What it does:** High-quality topographic map tiles from the German Federal Agency for Cartography. Used as LMoE's default online map layer.

**No key required.** The BKG TopPlusOpen service is free for non-commercial use.

Website: **https://gdz.bkg.bund.de/index.php/default/topplus-open.html**

---

## Offline Map Tiles (PMTiles)

**Not an API** — these are files you download once and store locally for offline map use.

1. Go to **https://protomaps.com/downloads**
2. Select your region (e.g. Europe, or a specific country)
3. Download the `.pmtiles` file — sizes range from a few hundred MB to several GB depending on region
4. In LMoE: **Settings → Map → Load PMTiles file**

Once loaded, the map works fully offline using the local tile file.

---

## Summary

| Service | Key needed | Where to get it | Free? |
|---|---|---|---|
| AISStream | Yes (recommended) | https://aisstream.io | ✓ |
| OpenSky Network | Optional | https://opensky-network.org | ✓ |
| Open-Meteo | No | https://open-meteo.com | ✓ |
| Nominatim | No | Automatic | ✓ |
| BKG TopPlusOpen | No | Automatic | ✓ |
| PMTiles (offline maps) | N/A — file download | https://protomaps.com/downloads | ✓ |
