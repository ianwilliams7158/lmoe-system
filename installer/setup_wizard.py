#!/usr/bin/env python3
"""
LMoE Setup Wizard
=================
Cross-platform interactive setup wizard. Runs automatically after
install_linux.sh or install_windows.ps1 completes, or can be re-run
at any time with:

    python3 setup_wizard.py

What it does
------------
  1.  Welcome and overview
  2.  Operator name
  3.  Home location (search by name or enter coordinates)
  4.  API keys — AISStream (ship watch), OpenSky (flight watch)
  5.  ZIM library path
  6.  Espy firmware flash (optional)
  7.  Writes lmoe_config.json
  8.  Patches lmoe.html with operator name, coordinates, API keys
  9.  Configures and starts systemd services (Linux) or startup tasks (Windows)
  10. Opens LMoE in Chrome
  11. Download links for maps, Kiwix, and other resources
"""

import json
import os
import platform
import re
import shutil
import subprocess
import sys
import textwrap
import time
import urllib.parse
import urllib.request
from pathlib import Path

# ── Constants ─────────────────────────────────────────────────────────────────
WIZARD_VERSION  = '1.0'
REPO_BASE       = 'https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main'
IS_WINDOWS      = platform.system() == 'Windows'
IS_LINUX        = platform.system() == 'Linux'
IS_PI           = IS_LINUX and Path('/proc/device-tree/model').exists()

# Paths — set during init
INSTALL_DIR     = Path(__file__).resolve().parent
LMOE_HTML       = INSTALL_DIR / 'lmoe.html'
CONFIG_FILE     = INSTALL_DIR / 'lmoe_config.json'
PROXY_SCRIPT    = INSTALL_DIR / 'lmoe_proxy.py'
SYNC_SCRIPT     = INSTALL_DIR / 'lmoe_sync_server.py'
ESPY_BIN        = INSTALL_DIR / 'espy' / 'firmware' / 'lmoe_espy.bin'

# Buckingham Palace defaults (used until user sets their location)
DEFAULT_LAT  = 51.5014
DEFAULT_LNG  = -0.1419
DEFAULT_NAME = 'Buckingham Palace, London'

# ── Colour helpers ────────────────────────────────────────────────────────────
def clr(code, text):
    if IS_WINDOWS:
        return text  # Windows console may not support ANSI — keep it plain
    return f'\033[{code}m{text}\033[0m'

def green(t):  return clr('32', t)
def yellow(t): return clr('33', t)
def cyan(t):   return clr('36', t)
def red(t):    return clr('31', t)
def bold(t):   return clr('1',  t)
def dim(t):    return clr('2',  t)

def hr(char='─', width=60):
    print(clr('2', char * width))

def banner(title):
    print()
    hr('═')
    print(bold(f'  {title}'))
    hr('═')

def section(title):
    print()
    hr()
    print(cyan(f'  ▸ {title}'))
    hr()

def ok(msg):    print(f'  {green("✓")} {msg}')
def warn(msg):  print(f'  {yellow("⚠")} {msg}')
def err(msg):   print(f'  {red("✗")} {msg}')
def info(msg):  print(f'  {dim("·")} {msg}')
def step(msg):  print(f'\n  {bold(msg)}')

def ask(prompt, default='', required=False):
    hint = f' [{default}]' if default else ''
    while True:
        val = input(f'  → {prompt}{hint}: ').strip()
        if not val and default:
            return default
        if val:
            return val
        if not required:
            return ''
        print(f'  {red("This field is required.")}')

def ask_yn(prompt, default='y'):
    hint = 'Y/n' if default == 'y' else 'y/N'
    val = input(f'  → {prompt} [{hint}]: ').strip().lower()
    if not val:
        return default == 'y'
    return val.startswith('y')

def wrap(text, width=56, indent='  '):
    return '\n'.join(
        indent + line
        for line in textwrap.wrap(text, width=width)
    )

# ── Config load/save ──────────────────────────────────────────────────────────
def config_load():
    if CONFIG_FILE.exists():
        try:
            with open(CONFIG_FILE) as f:
                return json.load(f)
        except Exception:
            pass
    return {
        'operator_name': '',
        'home_lat': DEFAULT_LAT,
        'home_lng': DEFAULT_LNG,
        'home_name': DEFAULT_NAME,
        'opensky_client_id': '',
        'opensky_client_secret': '',
        'aisstream_api_key': '',
        'zim_directory': '',
        'zim_index_path': '',
        'proxy_port': 5001,
        'proxy_autostart': True,
    }

def config_save(cfg):
    cfg['_comment'] = 'LMoE configuration — created by LMoE Setup Wizard'
    cfg['_version'] = WIZARD_VERSION
    tmp = str(CONFIG_FILE) + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, CONFIG_FILE)
    ok(f'Configuration saved to {CONFIG_FILE}')

# ── Location search via Nominatim ─────────────────────────────────────────────
def nominatim_search(query):
    """Search for a place name using Nominatim. Returns list of results."""
    try:
        url = 'https://nominatim.openstreetmap.org/search?' + urllib.parse.urlencode({
            'q': query, 'format': 'json', 'limit': 5,
            'addressdetails': 0,
        })
        req = urllib.request.Request(url,
              headers={'User-Agent': 'LMoE-Setup-Wizard/1.0'})
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read())
    except Exception as e:
        warn(f'Location search failed: {e}')
        return []

# ── LMoE HTML patching ───────────────────────────────────────────────────────
def patch_lmoe_html(cfg):
    """
    Patch lmoe.html with the user's settings — operator name, coordinates,
    API keys. All substitutions are idempotent.
    """
    if not LMOE_HTML.exists():
        err(f'lmoe.html not found at {LMOE_HTML}')
        return False

    with open(LMOE_HTML, 'r', encoding='utf-8') as f:
        html = f.read()

    name = cfg.get('operator_name', '') or 'OPERATOR'
    lat  = cfg.get('home_lat', DEFAULT_LAT)
    lng  = cfg.get('home_lng', DEFAULT_LNG)
    loc  = cfg.get('home_name', DEFAULT_NAME)
    aiskey = cfg.get('aisstream_api_key', '')

    # 1. Operator name in SETTINGS_DEFAULTS
    html = re.sub(
        r"(operatorName:\s*)'[^']*'",
        f"operatorName: '{name}'",
        html, count=1
    )

    # 2. Home coordinates in SETTINGS_DEFAULTS
    html = re.sub(
        r"(homeLat:\s*)[\d.\-]+",
        f"homeLat:       {lat}",
        html, count=1
    )
    html = re.sub(
        r"(homeLng:\s*)[\d.\-]+",
        f"homeLng:       {lng}",
        html, count=1
    )
    html = re.sub(
        r"(homeName:\s*)'[^']*'",
        f"homeName:      '{loc}'",
        html, count=1
    )

    # 3. Hidden settings inputs (initial HTML values)
    html = re.sub(
        r'(id="cfg-home-lat"\s+value=")[^"]*"',
        f'id="cfg-home-lat" value="{lat}"',
        html, count=1
    )
    html = re.sub(
        r'(id="cfg-home-lng"\s+value=")[^"]*"',
        f'id="cfg-home-lng" value="{lng}"',
        html, count=1
    )
    html = re.sub(
        r'(id="cfg-home-name"\s+value=")[^"]*"',
        f'id="cfg-home-name" value="{loc}"',
        html, count=1
    )

    # 4. AISStream API key
    if aiskey:
        html = re.sub(
            r"(const SW_DEFAULT_KEY\s*=\s*)'[^']*'",
            f"const SW_DEFAULT_KEY   = '{aiskey}'",
            html, count=1
        )

    # 5. Default map position (loraPos and map centre)
    html = re.sub(
        r'let loraPos\s*=\s*\{[^}]+\}',
        f'let loraPos = {{ lat: {lat}, lng: {lng} }}',
        html, count=1
    )

    # Write back
    backup = LMOE_HTML.with_suffix('.html.bak')
    shutil.copy(LMOE_HTML, backup)
    with open(LMOE_HTML, 'w', encoding='utf-8') as f:
        f.write(html)

    ok(f'lmoe.html patched (backup at {backup.name})')
    return True

# ── systemd service helpers (Linux) ──────────────────────────────────────────
def write_systemd_service(name, description, exec_cmd, working_dir):
    unit = f"""[Unit]
Description={description}
After=network.target

[Service]
Type=simple
WorkingDirectory={working_dir}
ExecStart={exec_cmd}
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
"""
    svc_path = Path(f'/etc/systemd/system/{name}.service')
    try:
        svc_path.write_text(unit)
        subprocess.run(['systemctl', 'daemon-reload'], check=True, capture_output=True)
        subprocess.run(['systemctl', 'enable', name],  check=True, capture_output=True)
        subprocess.run(['systemctl', 'restart', name], check=True, capture_output=True)
        ok(f'Service {name} installed and started')
        return True
    except PermissionError:
        warn(f'Cannot write to /etc/systemd — try running with sudo')
        return False
    except subprocess.CalledProcessError as e:
        warn(f'systemd error for {name}: {e}')
        return False

# ── Windows startup task helper ───────────────────────────────────────────────
def add_windows_startup(name, py_script):
    """Add a Python script to Windows startup via Task Scheduler."""
    python_exe = sys.executable
    cmd = (
        f'schtasks /create /tn "LMoE\\{name}" /tr '
        f'"\\\"{python_exe}\\\" \\\"{py_script}\\\"" '
        f'/sc onlogon /rl highest /f'
    )
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if result.returncode == 0:
            ok(f'Startup task {name} registered')
            return True
        else:
            warn(f'Task Scheduler error: {result.stderr.strip()}')
            return False
    except Exception as e:
        warn(f'Could not register startup task: {e}')
        return False

# ── Espy flash ────────────────────────────────────────────────────────────────
def find_serial_ports():
    """Return a list of likely ESP32 serial ports."""
    ports = []
    if IS_WINDOWS:
        import winreg
        try:
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                 r'HARDWARE\DEVICEMAP\SERIALCOMM')
            i = 0
            while True:
                try:
                    _, port, _ = winreg.EnumValue(key, i)
                    ports.append(port)
                    i += 1
                except OSError:
                    break
        except Exception:
            pass
    else:
        for pattern in ['/dev/ttyUSB*', '/dev/ttyACM*']:
            import glob
            ports.extend(sorted(glob.glob(pattern)))
    return ports

def flash_espy(port):
    """Flash lmoe_espy.bin to Espy via esptool."""
    if not ESPY_BIN.exists():
        err(f'Firmware not found: {ESPY_BIN}')
        return False

    try:
        import esptool
        esptool_available = True
    except ImportError:
        esptool_available = False

    esptool_cmd = 'esptool.py' if esptool_available else None
    # Try common install locations
    for candidate in ['esptool.py', 'esptool', str(Path(sys.executable).parent / 'esptool.py')]:
        if shutil.which(candidate):
            esptool_cmd = candidate
            break

    if not esptool_cmd:
        err('esptool not found — run: pip install esptool')
        return False

    print(f'\n  Flashing Espy on {port}...')
    print(dim('  (This takes 30–60 seconds. Do not disconnect Espy.)'))

    cmd = [
        esptool_cmd,
        '--port',     port,
        '--baud',     '921600',
        '--chip',     'esp32',
        'write_flash',
        '--flash_mode',  'dio',
        '--flash_freq',  '80m',
        '--flash_size',  '4MB',
        '0x0000',    str(ESPY_BIN),
    ]

    try:
        result = subprocess.run(cmd, capture_output=False, text=True)
        if result.returncode == 0:
            ok('Espy flashed successfully!')
            print(dim('  Espy will restart automatically.'))
            return True
        else:
            err('Flash failed — check connection and try again.')
            return False
    except FileNotFoundError:
        err(f'esptool not found at {esptool_cmd}')
        return False

# ── Steps ─────────────────────────────────────────────────────────────────────

def step_welcome():
    banner('LMoE Setup Wizard')
    print(f"""
  Welcome to {bold('Last Man on Earth')} — Survival Intelligence System.

  This wizard will configure LMoE for your system. It will:

    {green("✓")} Set your operator name and home location
    {green("✓")} Configure API keys for ship and flight tracking
    {green("✓")} Set up background services (proxy, sync server)
    {green("✓")} Optionally flash your Espy field terminal
    {green("✓")} Open LMoE in Chrome when done

  You can re-run this wizard at any time to change settings.
  All previous settings are preserved as defaults.

  {dim("Platform: " + platform.system() + " " + platform.machine())}
  {dim("Install:  " + str(INSTALL_DIR))}
""")
    input('  Press Enter to begin...')


def step_operator(cfg):
    section('Operator Identity')
    print(wrap(
        'This is the name LMoE uses for you throughout the system — '
        'your LoRa callsign, map marker, boot screen, and journal. '
        'Keep it short (max 10 characters for LoRa compatibility).'
    ))
    current = cfg.get('operator_name', '')
    name = ask('Operator name', default=current or '', required=True)
    name = name.upper()[:16]
    cfg['operator_name'] = name
    ok(f'Operator set to: {bold(name)}')


def step_location(cfg):
    section('Home Location')
    print(wrap(
        'Your home coordinates are used for map centring, satellite pass '
        'calculations, weather data, and LoRa beacon position. '
        'You can search by place name or enter coordinates directly.'
    ))

    current_name = cfg.get('home_name', DEFAULT_NAME)
    current_lat  = cfg.get('home_lat', DEFAULT_LAT)
    current_lng  = cfg.get('home_lng', DEFAULT_LNG)
    info(f'Current: {current_name} ({current_lat}, {current_lng})')

    method = ask('Search by name or enter coordinates? [name/coords]', default='name')

    if method.lower().startswith('c'):
        lat_str = ask('Latitude (decimal, e.g. 51.2441)',  default=str(current_lat), required=True)
        lng_str = ask('Longitude (decimal, e.g. -0.3067)', default=str(current_lng), required=True)
        loc_name = ask('Location name (for display)', default=current_name, required=True)
        try:
            cfg['home_lat']  = float(lat_str)
            cfg['home_lng']  = float(lng_str)
            cfg['home_name'] = loc_name
            ok(f'Location set: {loc_name} ({cfg["home_lat"]}, {cfg["home_lng"]})')
        except ValueError:
            warn('Invalid coordinates — keeping current location.')
    else:
        query = ask('Place name', default=current_name, required=True)
        print(f'  {dim("Searching...")}', end='', flush=True)
        results = nominatim_search(query)
        print()
        if not results:
            warn('No results found — keeping current location.')
            return
        print()
        for i, r in enumerate(results[:5], 1):
            lat = float(r.get('lat', 0))
            lng = float(r.get('lon', 0))
            name = r.get('display_name', '')[:72]
            print(f'  {cyan(str(i))}. {name}')
            print(f'     {dim(f"{lat:.5f}, {lng:.5f}")}')
        print()
        choice = ask(f'Select 1–{min(5,len(results))} (or press Enter to skip)', default='')
        if choice.isdigit() and 1 <= int(choice) <= len(results):
            r = results[int(choice) - 1]
            cfg['home_lat']  = float(r['lat'])
            cfg['home_lng']  = float(r['lon'])
            # Use a cleaned-up name
            raw_name = r.get('display_name', query)
            # Take the first two comma-separated parts for brevity
            parts = [p.strip() for p in raw_name.split(',')]
            cfg['home_name'] = ', '.join(parts[:2])
            ok(f'Location set: {cfg["home_name"]} ({cfg["home_lat"]:.5f}, {cfg["home_lng"]:.5f})')
        else:
            info('Keeping current location.')


def step_api_keys(cfg):
    section('API Keys')
    print(wrap(
        'LMoE uses two optional API keys for live tracking features. '
        'Both services have free tiers. Without them, tracking features '
        'run in limited anonymous mode. Press Enter to skip any key.'
    ))

    # ── AISStream ────────────────────────────────────────────────────────────
    step('AISStream — Live Ship Tracking')
    print(wrap(
        'AISStream provides a free WebSocket feed of AIS ship position data '
        'worldwide. Without a key, ship watch is limited to a small number '
        'of anonymous requests per day.'
    ))
    print(f'\n  {dim("Get a free key at:")} {cyan("https://aisstream.io")}')
    print(f'  {dim("Sign up → Dashboard → API Keys → Create Key")}')
    print(f'  {dim("Free tier: unlimited vessels, global coverage")}')
    current = cfg.get('aisstream_api_key', '')
    key = ask('AISStream API key', default=current or '')
    if key:
        cfg['aisstream_api_key'] = key.strip()
        ok('AISStream key saved.')
    else:
        info('Skipped — ship watch will use anonymous mode.')

    # ── OpenSky ──────────────────────────────────────────────────────────────
    step('OpenSky Network — Live Flight Tracking')
    print(wrap(
        'OpenSky provides real-time flight position data. A free account '
        'gives significantly higher API rate limits than anonymous access. '
        'The LMoE proxy server handles authentication locally.'
    ))
    print(f'\n  {dim("Get free credentials at:")} {cyan("https://opensky-network.org")}')
    print(f'  {dim("Register → My OpenSky → API Keys → Create Client")}')
    print(f'  {dim("Free tier: 400 API credits/day (vs 100 anonymous)")}')
    current_id  = cfg.get('opensky_client_id', '')
    current_sec = cfg.get('opensky_client_secret', '')
    client_id   = ask('OpenSky Client ID',     default=current_id  or '')
    client_sec  = ask('OpenSky Client Secret', default=current_sec or '')
    if client_id and client_sec:
        cfg['opensky_client_id']     = client_id.strip()
        cfg['opensky_client_secret'] = client_sec.strip()
        ok('OpenSky credentials saved.')
    else:
        info('Skipped — flight watch will use anonymous mode (100 req/day).')


def step_zim(cfg):
    section('ZIM Library (Offline Knowledge Base)')
    print(wrap(
        'LMoE can search your Kiwix ZIM files (offline Wikipedia, '
        'medical references, survival guides, etc.) from the Intelligence '
        'panel. This step is optional — you can configure it later in '
        'LMoE → Settings → ZIM Library.'
    ))
    current = cfg.get('zim_directory', '')
    print(f'\n  {dim("Download ZIM files at:")} {cyan("https://library.kiwix.org")}')
    zim_dir = ask('Path to your ZIM files folder', default=current or '')
    if zim_dir:
        p = Path(zim_dir).expanduser()
        if p.is_dir():
            cfg['zim_directory'] = str(p)
            zims = list(p.glob('*.zim'))
            ok(f'ZIM directory set: {p}  ({len(zims)} ZIM file{"s" if len(zims)!=1 else ""} found)')
            if zims and ask_yn('Run ZIM indexer now? (needed for Intelligence panel search)', default='y'):
                step_zim_index(cfg, p)
        else:
            warn(f'Directory not found: {p} — skipping ZIM setup.')
    else:
        info('Skipped — configure later in Settings → ZIM Library.')


def step_zim_index(cfg, zim_dir):
    indexer = INSTALL_DIR / 'lmoe_zim_indexer.py'
    if not indexer.exists():
        warn('lmoe_zim_indexer.py not found — skipping indexing.')
        return
    index_path = INSTALL_DIR / 'lmoe_zim_index.json'
    print(f'\n  {dim("Indexing ZIM files in")} {zim_dir}')
    print(  f'  {dim("This can take a while for large files. Ctrl+C stops safely.")}')
    print(  f'  {dim("Progress is saved after each ZIM — safe to interrupt and resume.")}')
    try:
        subprocess.run(
            [sys.executable, str(indexer),
             '--zim-dir', str(zim_dir),
             '--output',  str(index_path)],
            check=False  # non-fatal if interrupted
        )
        if index_path.exists():
            cfg['zim_index_path'] = str(index_path)
            ok(f'ZIM index written to {index_path}')
        else:
            warn('ZIM index not written — run lmoe_zim_indexer.py manually.')
    except KeyboardInterrupt:
        if index_path.exists():
            cfg['zim_index_path'] = str(index_path)
            ok('ZIM indexing interrupted — partial index saved.')


def step_services(cfg):
    section('Background Services')
    print(wrap(
        'LMoE uses two background services: the OpenSky proxy '
        '(for authenticated flight tracking) and the Espy sync server '
        '(for the Espy field terminal). Both are optional but recommended.'
    ))

    if IS_LINUX:
        _step_services_linux(cfg)
    elif IS_WINDOWS:
        _step_services_windows(cfg)
    else:
        warn('Unsupported platform — start services manually:')
        info(f'python3 {PROXY_SCRIPT}')
        info(f'python3 {SYNC_SCRIPT}')


def _step_services_linux(cfg):
    step('Installing systemd services (requires sudo)')
    if os.geteuid() != 0:
        warn('Not running as root. Services will not be installed.')
        warn(f'Re-run with: sudo python3 {Path(__file__).name}')
        warn('Or start services manually:')
        info(f'  python3 {PROXY_SCRIPT} &')
        info(f'  python3 {SYNC_SCRIPT} &')
        return

    python = sys.executable

    write_systemd_service(
        name        = 'lmoe-proxy',
        description = 'LMoE OpenSky Proxy',
        exec_cmd    = f'{python} {PROXY_SCRIPT}',
        working_dir = str(INSTALL_DIR),
    )
    write_systemd_service(
        name        = 'lmoe-sync',
        description = 'LMoE Espy Sync Server',
        exec_cmd    = f'{python} {SYNC_SCRIPT}',
        working_dir = str(INSTALL_DIR),
    )


def _step_services_windows(cfg):
    step('Registering startup tasks')
    add_windows_startup('LMoE-Proxy', str(PROXY_SCRIPT))
    add_windows_startup('LMoE-Sync',  str(SYNC_SCRIPT))
    # Start them now too
    for script in (PROXY_SCRIPT, SYNC_SCRIPT):
        try:
            subprocess.Popen(
                [sys.executable, str(script)],
                creationflags=subprocess.CREATE_NO_WINDOW,
                cwd=str(INSTALL_DIR),
            )
            ok(f'Started {script.name}')
        except Exception as e:
            warn(f'Could not start {script.name}: {e}')


def step_espy(cfg):
    section('Espy Field Terminal (Optional)')
    print(wrap(
        'Espy is the companion device that goes with you on supply runs. '
        'If you have an Espy (ESP32-2432S028R / CYD), plug it in now '
        'and we will flash it with the latest firmware automatically. '
        'You can also skip this and flash it later.'
    ))

    if not ESPY_BIN.exists():
        warn(f'Firmware file not found: {ESPY_BIN}')
        warn('Download it from: https://github.com/ianwilliams7158/lmoe-system')
        info('Skipping Espy setup.')
        return

    if not ask_yn('Flash Espy now?', default='y'):
        info('Skipped — flash Espy later via: python3 setup_wizard.py')
        return

    print(wrap(
        'Connect Espy to this computer via USB. '
        'Use the USB port on the back of the CYD, not the charging port.'
    ))
    input('  Press Enter when Espy is connected...')
    time.sleep(1)

    ports = find_serial_ports()
    if not ports:
        warn('No serial ports detected.')
        warn('Make sure Espy is connected and drivers are installed.')
        if IS_WINDOWS:
            info('Windows: install CP2102 driver from Silicon Labs if needed.')
        else:
            info('Linux: add yourself to the dialout group: sudo usermod -aG dialout $USER')
        return

    if len(ports) == 1:
        port = ports[0]
        info(f'Found port: {port}')
    else:
        print('\n  Available serial ports:')
        for i, p in enumerate(ports, 1):
            print(f'  {cyan(str(i))}. {p}')
        choice = ask(f'Select port [1–{len(ports)}]', default='1')
        try:
            port = ports[int(choice) - 1]
        except (ValueError, IndexError):
            port = ports[0]

    flash_espy(port)


def step_summary_and_links(cfg):
    section('Download Links')
    print(f"""
  The following items are needed for full LMoE functionality.
  Download them at your convenience and configure them in LMoE settings.

  {bold("Kiwix Desktop")} — open ZIM files from the Intelligence panel
    {cyan("https://www.kiwix.org/en/downloads/")}
    Linux:   sudo apt install kiwix-desktop
    Windows: download the installer from the link above

  {bold("ZIM Files")} — offline knowledge base (Wikipedia, survival guides, etc.)
    {cyan("https://library.kiwix.org")}
    Recommended: wikipedia_en_all_nopic (maps well to survival research)
                 wikibooks_en_all        (how-to guides)
                 first_aid              (medical reference)

  {bold("Offline Map Tiles (PMTiles)")} — for offline map use without internet
    {cyan("https://protomaps.com/downloads")}
    Download the region covering your area of operations.
    Load in LMoE → Settings → Map → PMTiles

  {bold("AISStream")} — ship tracking API key (free)
    {cyan("https://aisstream.io")}

  {bold("OpenSky Network")} — flight tracking credentials (free)
    {cyan("https://opensky-network.org")}

  {bold("Open-Meteo")} — weather (no key needed, free and open)
    {cyan("https://open-meteo.com")}
    LMoE uses this automatically — no setup required.
""")


def step_write_and_patch(cfg):
    section('Writing Configuration')
    config_save(cfg)
    step('Patching lmoe.html...')
    patch_lmoe_html(cfg)


def step_open_lmoe():
    section('Launch LMoE')
    print(wrap(
        'Setup is complete. LMoE will now open in your default browser. '
        'On first load, go to Settings → Identity and confirm your details, '
        'then set your Day Zero date to start the survival day counter.'
    ))

    if not ask_yn('Open LMoE now?', default='y'):
        info(f'Open manually: {LMOE_HTML}')
        return

    import webbrowser
    url = LMOE_HTML.as_uri()
    webbrowser.open(url)
    ok(f'Opened: {url}')


def step_done(cfg):
    banner('Setup Complete')
    name = cfg.get('operator_name', 'OPERATOR')
    loc  = cfg.get('home_name', DEFAULT_NAME)
    print(f"""
  {green("LMoE is configured and ready.")}

  Operator : {bold(name)}
  Location : {loc}
  Config   : {CONFIG_FILE}

  {dim("To re-run setup:")}
  {dim(f"  python3 {Path(__file__).name}")}

  {bold("Godspeed, " + name + ".")}
""")


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    # Enable ANSI colour on Windows 10+
    if IS_WINDOWS:
        try:
            import ctypes
            ctypes.windll.kernel32.SetConsoleMode(
                ctypes.windll.kernel32.GetStdHandle(-11), 7
            )
        except Exception:
            pass

    cfg = config_load()

    try:
        step_welcome()
        step_operator(cfg)
        step_location(cfg)
        step_api_keys(cfg)
        step_zim(cfg)
        step_write_and_patch(cfg)
        step_services(cfg)
        step_espy(cfg)
        step_summary_and_links(cfg)
        step_open_lmoe()
        step_done(cfg)

    except KeyboardInterrupt:
        print(f'\n\n  {yellow("Setup interrupted.")}')
        if ask_yn('Save progress so far?', default='y'):
            config_save(cfg)
            patch_lmoe_html(cfg)
        print(f'  Re-run: {bold(f"python3 {Path(__file__).name}")}')
        sys.exit(0)


if __name__ == '__main__':
    main()
