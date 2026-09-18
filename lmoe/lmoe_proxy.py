#!/usr/bin/env python3
"""
LMoE Proxy Server
=================
A lightweight local HTTP proxy that sits between LMoE (running in Chrome)
and the OpenSky Network API. It exists for two reasons:

  1. OpenSky's API requires credentials — passing them through the browser
     exposes them in localStorage and network requests. The proxy keeps them
     server-side.

  2. OpenSky's API does not send CORS headers that allow browser requests from
     file:// origins. The proxy handles the request server-side and returns
     the data with correct CORS headers.

The proxy listens on http://localhost:5001 and forwards:
  GET /api/states/all  →  https://opensky-network.org/api/states/all

USAGE
-----
  python3 lmoe_proxy.py
  python3 lmoe_proxy.py --client-id YOUR_ID --client-secret YOUR_SECRET

  Or set credentials via environment variables:
    OPENSKY_CLIENT_ID=your_id
    OPENSKY_CLIENT_SECRET=your_secret

  Or configure them in lmoe_config.json (created by the LMoE installer).

The proxy runs in the foreground. On Linux it is installed as a systemd
service by the installer. On Windows it is installed as a startup task.
"""

import argparse
import json
import os
import sys
import time
import urllib.request
import urllib.parse
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler

# ── Config ────────────────────────────────────────────────────────────────────
DEFAULT_PORT        = 5001
OPENSKY_BASE        = 'https://opensky-network.org/api'
CONFIG_FILE         = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lmoe_config.json')
CACHE_TTL           = 10   # seconds — OpenSky free tier rate limit is 10s
ALLOWED_ORIGINS     = ['*']

# ── Globals ───────────────────────────────────────────────────────────────────
_credentials = {'client_id': '', 'client_secret': ''}
_cache        = {}  # path → (timestamp, data_bytes)


def load_config():
    """Load credentials from lmoe_config.json if it exists."""
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, 'r') as f:
                cfg = json.load(f)
            _credentials['client_id']     = cfg.get('opensky_client_id', '')
            _credentials['client_secret'] = cfg.get('opensky_client_secret', '')
            if _credentials['client_id']:
                print(f"  Loaded OpenSky credentials from {CONFIG_FILE}")
        except Exception as e:
            print(f"  Warning: could not read {CONFIG_FILE}: {e}")


def fetch_opensky(path: str, query: str) -> tuple[int, bytes, str]:
    """
    Forward a request to OpenSky. Returns (status_code, body_bytes, content_type).
    Uses basic auth if credentials are configured, otherwise anonymous.
    Results are cached for CACHE_TTL seconds.
    """
    cache_key = path + '?' + query
    now = time.time()
    if cache_key in _cache:
        ts, data = _cache[cache_key]
        if now - ts < CACHE_TTL:
            return 200, data, 'application/json'

    url = OPENSKY_BASE + path
    if query:
        url += '?' + query

    req = urllib.request.Request(url)
    req.add_header('User-Agent', 'LMoE/6.0 (local proxy)')

    cid = _credentials.get('client_id', '')
    csec = _credentials.get('client_secret', '')
    if cid and csec:
        import base64
        token = base64.b64encode(f'{cid}:{csec}'.encode()).decode()
        req.add_header('Authorization', f'Basic {token}')

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = resp.read()
            _cache[cache_key] = (now, data)
            return 200, data, resp.headers.get('Content-Type', 'application/json')
    except urllib.error.HTTPError as e:
        body = e.read()
        return e.code, body, 'application/json'
    except urllib.error.URLError as e:
        msg = json.dumps({'error': str(e.reason)}).encode()
        return 503, msg, 'application/json'
    except Exception as e:
        msg = json.dumps({'error': str(e)}).encode()
        return 500, msg, 'application/json'


class ProxyHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Suppress default access log spam; only log errors
        pass

    def _cors(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type, Authorization')

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        parsed  = urllib.parse.urlparse(self.path)
        path    = parsed.path
        query   = parsed.query

        # Health check endpoint — LMoE uses this to detect if proxy is running
        if path == '/' or path == '/health':
            body = json.dumps({
                'status': 'ok',
                'service': 'LMoE Proxy',
                'authenticated': bool(_credentials.get('client_id')),
            }).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self._cors()
            self.end_headers()
            self.wfile.write(body)
            return

        # OpenSky state vectors endpoint
        if path.startswith('/api/states'):
            os_path = path[4:]  # strip /api prefix → /states/all
            status, body, ctype = fetch_opensky(os_path, query)
            self.send_response(status)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self._cors()
            self.end_headers()
            self.wfile.write(body)
            auth_note = '(auth)' if _credentials.get('client_id') else '(anon)'
            print(f"  {status} {path} {auth_note}")
            return

        # Anything else — 404
        body = json.dumps({'error': 'not found'}).encode()
        self.send_response(404)
        self.send_header('Content-Type', 'application/json')
        self._cors()
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description='LMoE local proxy for OpenSky Network API')
    parser.add_argument('--port',          type=int, default=DEFAULT_PORT,
                        help=f'Port to listen on (default: {DEFAULT_PORT})')
    parser.add_argument('--client-id',     default='',
                        help='OpenSky client ID (overrides config file / env var)')
    parser.add_argument('--client-secret', default='',
                        help='OpenSky client secret (overrides config file / env var)')
    args = parser.parse_args()

    print('LMoE Proxy Server')
    print('─' * 40)

    # Priority: CLI args > env vars > config file
    load_config()
    if os.environ.get('OPENSKY_CLIENT_ID'):
        _credentials['client_id']     = os.environ['OPENSKY_CLIENT_ID']
        _credentials['client_secret'] = os.environ.get('OPENSKY_CLIENT_SECRET', '')
        print(f"  Loaded OpenSky credentials from environment")
    if args.client_id:
        _credentials['client_id']     = args.client_id
        _credentials['client_secret'] = args.client_secret
        print(f"  Loaded OpenSky credentials from command line")

    if _credentials['client_id']:
        print(f"  Mode: Authenticated (client_id: {_credentials['client_id'][:8]}...)")
    else:
        print(f"  Mode: Anonymous (OpenSky rate limits apply — 100 requests/day)")
        print(f"  To authenticate: add OpenSky credentials to {CONFIG_FILE}")

    server = HTTPServer(('127.0.0.1', args.port), ProxyHandler)
    print(f"  Listening on http://127.0.0.1:{args.port}")
    print(f"  LMoE will connect automatically when Flight Watch is opened")
    print(f"  Press Ctrl+C to stop\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nProxy stopped.')


if __name__ == '__main__':
    main()
