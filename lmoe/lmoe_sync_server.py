#!/usr/bin/env python3
"""
LMoE <-> Espy Sync Server
==========================

Small standalone HTTP server providing the backend endpoints Espy's
firmware talks to. Run this on LMoE-main (the Pi 4B), alongside the
existing static file server / Flight Watch proxy.

Espy is ALWAYS the HTTP client — this server never initiates a
connection to Espy. "On-demand sync" from LMoE's System Panel works by
setting a flag here; Espy picks it up on its next periodic check-in
(within ~60s) and performs a full sync there and then.

ENDPOINTS
---------
POST /api/espy/status
    Espy posts its current status (GPS, WiFi, pending counts) here every
    ~60s. Response includes "sync_requested": true/false — Espy should
    perform a full sync if true, and the flag is cleared once Espy does.

GET /api/espy/mission
GET /api/espy/wanted
GET /api/espy/resources
    Espy pulls these to refresh its SD card copies before a run.
    Returns the current contents of data/lmoe/<name>.json.

GET /api/espy/markers
    Returns KML markers from LMoE's lmoe_kml_markers_v2 localStorage
    snapshot (written by LMoE to data/lmoe/markers.json during sync).
    Photos are STRIPPED — base64 photo fields are removed before sending
    to Espy to prevent ESP32 heap exhaustion. All other marker fields
    (name, cat, lat, lng, notes, status, date) are preserved.

POST /api/espy/sync/push
    Espy pushes accumulated field data here after a run:
      { "markers": [...], "journal": [...], "wanted_updates": [...] }
    Each marker/journal entry is appended to data/field/markers/ or
    data/field/journal/ as individual timestamped files. wanted_updates
    are applied directly to data/lmoe/wanted.json (marking items found).

POST /api/espy/request-sync
    Called by LMoE's System Panel "QUICK SYNC" button. Sets the
    sync_requested flag, returned to Espy on its next status POST.

GET /api/espy/last-status
    Returns the most recently received Espy status (for the System
    Panel to display without waiting for its own poll).

RUNNING
-------
    python3 lmoe_sync_server.py [port]

Default port: 8080 (matches Espy's LMOE_PORT in config.h).
Data is stored under ./data/ relative to this script.
"""

import json
import os
import sys
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BASE_DIR  = Path(__file__).resolve().parent
DATA_DIR  = BASE_DIR / "data"
LMOE_DIR  = DATA_DIR / "lmoe"       # mission.json, wanted.json, resources.json, markers.json
FIELD_DIR = DATA_DIR / "field"      # markers/, journal/, wanted_updates log

for d in (LMOE_DIR, FIELD_DIR / "markers", FIELD_DIR / "journal"):
    d.mkdir(parents=True, exist_ok=True)

# In-memory state (lost on restart — fine for this use case)
state = {
    "sync_requested": False,
    "last_status":    None,
    "last_status_at": None,
}

# Fields that are safe to send to Espy — photos and base64 blobs excluded.
# Espy's ESP32 has ~320KB heap; a single base64-encoded photo can be 200KB+.
MARKER_ESPY_FIELDS = {"id", "name", "cat", "lat", "lng", "notes", "status", "date"}


def strip_marker_for_espy(marker):
    """Return a copy of marker dict with only Espy-safe fields. No photos."""
    return {k: v for k, v in marker.items() if k in MARKER_ESPY_FIELDS}


def read_json_file(path, default=None):
    try:
        with open(path, "r") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return default if default is not None else {}


def write_json_file(path, data):
    tmp = str(path) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)


class SyncHandler(SimpleHTTPRequestHandler):
    server_version = "LMoESync/1.1"

    def __init__(self, *args, **kwargs):
        # Serve static files (LMoE.html etc.) from the www/ directory
        super().__init__(*args, directory=str(BASE_DIR / "www"), **kwargs)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self):
        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return None

    # ------------------------------------------------------------
    # GET
    # ------------------------------------------------------------
    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/api/espy/mission":
            data = read_json_file(LMOE_DIR / "mission.json", {"run": {}})
            self._send_json(200, data)

        elif path == "/api/espy/wanted":
            data = read_json_file(LMOE_DIR / "wanted.json", {"items": []})
            self._send_json(200, data)

        elif path == "/api/espy/resources":
            data = read_json_file(LMOE_DIR / "resources.json", {"items": []})
            self._send_json(200, data)

        elif path == "/api/espy/markers":
            # Serve KML markers to Espy — photos stripped to protect ESP32 heap.
            # LMoE writes data/lmoe/markers.json during sync containing all
            # user-placed KML markers from its localStorage (lmoe_kml_markers_v2).
            raw = read_json_file(LMOE_DIR / "markers.json", {"markers": []})
            markers = raw.get("markers", [])
            safe = [strip_marker_for_espy(m) for m in markers]
            self._send_json(200, {"markers": safe, "count": len(safe)})

        elif path == "/api/espy/map-image":
            # Stream BMP image to Espy
            map_bmp = LMOE_DIR / "map.bmp"
            if not map_bmp.exists():
                self._send_json(404, {"error": "no map image available"})
                return
            data = map_bmp.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "image/bmp")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)

        elif path == "/api/espy/map-meta":
            data = read_json_file(LMOE_DIR / "map-meta.json", {})
            self._send_json(200, data)
            self._send_json(200, {
                "status":      state["last_status"],
                "received_at": state["last_status_at"],
            })

        elif path == "/api/espy/health":
            self._send_json(200, {"ok": True, "time": time.time()})

        elif path.startswith("/api/"):
            self._send_json(404, {"error": "not found"})

        else:
            # Not an API path — serve as a static file from www/
            super().do_GET()

    # ------------------------------------------------------------
    # POST
    # ------------------------------------------------------------
    def do_POST(self):
        path = self.path.split("?")[0]

        if path == "/api/espy/status":
            payload = self._read_json_body()
            if payload is None:
                self._send_json(400, {"error": "invalid json"})
                return

            state["last_status"]    = payload
            state["last_status_at"] = time.time()

            sync_now = state["sync_requested"]
            state["sync_requested"] = False

            self._send_json(200, {
                "ok":             True,
                "sync_requested": sync_now,
            })

        elif path == "/api/espy/map-image":
            # Receive BMP from LMoE, write to disk for Espy to pull
            length = int(self.headers.get("Content-Length", 0))
            if length == 0:
                self._send_json(400, {"error": "no data"})
                return
            data = self.rfile.read(length)
            (LMOE_DIR / "map.bmp").write_bytes(data)
            self._send_json(200, {"ok": True, "bytes": length})

        elif path == "/api/espy/map-meta":
            # Receive map metadata JSON from LMoE
            payload = self._read_json_body()
            if payload is None:
                self._send_json(400, {"error": "invalid json"})
                return
            write_json_file(LMOE_DIR / "map-meta.json", payload)
            self._send_json(200, {"ok": True})

        elif path == "/api/espy/request-sync":
            state["sync_requested"] = True
            self._send_json(200, {"ok": True, "sync_requested": True})

        elif path == "/api/espy/sync/push":
            payload = self._read_json_body()
            if payload is None:
                self._send_json(400, {"error": "invalid json"})
                return

            result = self._handle_sync_push(payload)
            self._send_json(200, result)

        elif path == "/api/espy/sync/lmoe-markers":
            # LMoE posts its full KML marker set here during a sync so the
            # server has an up-to-date copy to serve back to Espy.
            payload = self._read_json_body()
            if payload is None:
                self._send_json(400, {"error": "invalid json"})
                return
            markers = payload.get("markers", [])
            write_json_file(LMOE_DIR / "markers.json", {"markers": markers})
            self._send_json(200, {"ok": True, "markers_stored": len(markers)})

        else:
            self._send_json(404, {"error": "not found"})

    # ------------------------------------------------------------
    # Sync push handling
    # ------------------------------------------------------------
    def _handle_sync_push(self, payload):
        markers        = payload.get("markers", [])
        journal        = payload.get("journal", [])
        wanted_updates = payload.get("wanted_updates", [])

        markers_written = 0
        for m in markers:
            mid = m.get("id")
            if not mid:
                continue
            # Strip photos from Espy-originated markers too (shouldn't have
            # them, but belt-and-braces)
            safe_m = strip_marker_for_espy(m)
            # Preserve Espy-specific fields not in MARKER_ESPY_FIELDS
            for field in ("dropped_at", "gps_accuracy", "reviewed"):
                if field in m:
                    safe_m[field] = m[field]
            write_json_file(FIELD_DIR / "markers" / f"{mid}.json", safe_m)
            markers_written += 1

        journal_written = 0
        for j in journal:
            jid = j.get("id")
            if not jid:
                continue
            write_json_file(FIELD_DIR / "journal" / f"{jid}.json", j)
            journal_written += 1

        # Apply wanted_updates to the master wanted.json
        wanted_applied = 0
        if wanted_updates:
            wanted_data = read_json_file(LMOE_DIR / "wanted.json", {"items": []})
            items = wanted_data.get("items", [])
            by_id = {item.get("id"): item for item in items}

            for upd in wanted_updates:
                item_id = upd.get("id")
                if item_id and item_id in by_id:
                    by_id[item_id]["found"] = True
                    for field in ("found_at_lat", "found_at_lng",
                                  "found_at_name", "quantity_found", "notes"):
                        if field in upd:
                            by_id[item_id][field] = upd[field]
                    wanted_applied += 1

            wanted_data["items"] = list(by_id.values())
            write_json_file(LMOE_DIR / "wanted.json", wanted_data)

        return {
            "ok":                    True,
            "markers_received":      markers_written,
            "journal_received":      journal_written,
            "wanted_updates_applied": wanted_applied,
        }


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    server = ThreadingHTTPServer(("0.0.0.0", port), SyncHandler)
    print(f"LMoE sync server listening on :{port}")
    print(f"Data directory: {DATA_DIR}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
