#!/usr/bin/env bash
# ==============================================================================
# LMoE — Last Man on Earth — Linux / Raspberry Pi Installer
# ==============================================================================
#
# Single-command install:
#   curl -sSL https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main/installer/install_linux.sh | bash
#
# Or download and run:
#   wget https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main/installer/install_linux.sh
#   chmod +x install_linux.sh && ./install_linux.sh
#
# What this script does:
#   1.  Detects the platform (Raspberry Pi OS, Ubuntu, Debian)
#   2.  Installs system packages: python3, pip, git, chromium-browser, esptool
#   3.  Creates the LMoE install directory at ~/lmoe
#   4.  Downloads all LMoE files from GitHub
#   5.  Installs Python dependencies (libzim, beautifulsoup4, etc.)
#   6.  Creates a desktop shortcut and a 'lmoe' terminal command
#   7.  Launches the LMoE Setup Wizard to configure everything
#
# Tested on: Raspberry Pi OS (Bookworm 64-bit), Ubuntu 22.04, Debian 12
# ==============================================================================

set -euo pipefail

# ── Constants ──────────────────────────────────────────────────────────────────
REPO_BASE="https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main"
REPO_URL="https://github.com/ianwilliams7158/lmoe-system"
INSTALL_DIR="$HOME/lmoe"
PYTHON_MIN="3.9"
LMOE_VERSION="6.0"

# ── Colours ────────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'

ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
err()  { echo -e "  ${RED}✗${NC} $1"; }
info() { echo -e "  ${DIM}·${NC} $1"; }
step() { echo -e "\n${BOLD}  ▸ $1${NC}"; echo -e "  $(printf '─%.0s' {1..56})"; }
banner() {
  echo -e "\n${CYAN}$(printf '═%.0s' {1..60})${NC}"
  echo -e "${BOLD}  $1${NC}"
  echo -e "${CYAN}$(printf '═%.0s' {1..60})${NC}\n"
}

# ── Platform detection ─────────────────────────────────────────────────────────
detect_platform() {
  IS_PI=false
  IS_ARM=false
  ARCH=$(uname -m)
  OS_ID=$(. /etc/os-release 2>/dev/null && echo "$ID" || echo "unknown")
  OS_VER=$(. /etc/os-release 2>/dev/null && echo "$VERSION_ID" || echo "0")

  if [[ -f /proc/device-tree/model ]]; then
    PI_MODEL=$(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')
    IS_PI=true
    info "Detected: $PI_MODEL"
  fi

  if [[ "$ARCH" == arm* || "$ARCH" == aarch64 ]]; then
    IS_ARM=true
  fi

  info "OS: $OS_ID $OS_VER ($ARCH)"
}

# ── Dependency check ───────────────────────────────────────────────────────────
check_python() {
  if ! command -v python3 &>/dev/null; then
    err "python3 not found — will be installed."
    return 1
  fi
  PY_VER=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
  PY_OK=$(python3 -c "import sys; print(1 if sys.version_info >= (3,9) else 0)")
  if [[ "$PY_OK" == "1" ]]; then
    ok "Python $PY_VER"
    return 0
  else
    warn "Python $PY_VER found — LMoE requires 3.9+. Attempting upgrade."
    return 1
  fi
}

# ── Package install ────────────────────────────────────────────────────────────
install_packages() {
  step "Installing system packages"
  info "Updating package lists..."
  sudo apt-get update -qq

  PACKAGES=(
    python3
    python3-pip
    python3-venv
    git
    wget
    curl
  )

  # Chromium browser
  if $IS_PI; then
    PACKAGES+=(chromium-browser)
  else
    if apt-cache show chromium-browser &>/dev/null 2>&1; then
      PACKAGES+=(chromium-browser)
    elif apt-cache show chromium &>/dev/null 2>&1; then
      PACKAGES+=(chromium)
    else
      warn "Chromium not found in apt — install it manually if needed."
    fi
  fi

  # esptool for Espy flashing — try apt first, pip fallback later
  if apt-cache show esptool &>/dev/null 2>&1; then
    PACKAGES+=(esptool)
  fi

  info "Installing: ${PACKAGES[*]}"
  sudo apt-get install -y "${PACKAGES[@]}" 2>&1 | grep -E 'Get:|Setting up|already' || true

  ok "System packages installed"
}

# ── Python packages ────────────────────────────────────────────────────────────
install_python_packages() {
  step "Installing Python packages"

  PIP_PACKAGES=(
    "libzim>=3.5.0"
    "beautifulsoup4"
    "requests"
    "esptool"
  )

  for pkg in "${PIP_PACKAGES[@]}"; do
    name="${pkg%%[>=<]*}"
    info "Installing $name..."
    if python3 -m pip install "$pkg" --break-system-packages -q 2>/dev/null; then
      ok "$name"
    else
      # Fallback: user install
      if python3 -m pip install "$pkg" --user -q 2>/dev/null; then
        ok "$name (user install)"
      else
        warn "Could not install $name — some features may be limited."
      fi
    fi
  done
}

# ── Download LMoE files ────────────────────────────────────────────────────────
download_files() {
  step "Downloading LMoE files"
  mkdir -p "$INSTALL_DIR"
  mkdir -p "$INSTALL_DIR/espy/firmware"
  mkdir -p "$INSTALL_DIR/data/lmoe"
  mkdir -p "$INSTALL_DIR/data/field/markers"
  mkdir -p "$INSTALL_DIR/data/field/journal"
  mkdir -p "$INSTALL_DIR/www"

  FILES=(
    "lmoe/lmoe.html                      $INSTALL_DIR/lmoe.html"
    "lmoe/lmoe_config.json               $INSTALL_DIR/lmoe_config.json"
    "lmoe/lmoe_proxy.py                  $INSTALL_DIR/lmoe_proxy.py"
    "lmoe/lmoe_sync_server.py            $INSTALL_DIR/lmoe_sync_server.py"
    "lmoe/lmoe_zim_indexer.py            $INSTALL_DIR/lmoe_zim_indexer.py"
    "espy/firmware/main.cpp              $INSTALL_DIR/espy/firmware/main.cpp"
    "espy/firmware/platformio.ini        $INSTALL_DIR/espy/firmware/platformio.ini"
    "installer/setup_wizard.py           $INSTALL_DIR/setup_wizard.py"
  )

  # Espy binary — non-fatal if not yet uploaded
  OPTIONAL_FILES=(
    "espy/firmware/lmoe_espy.bin         $INSTALL_DIR/espy/firmware/lmoe_espy.bin"
  )

  for entry in "${FILES[@]}"; do
    src=$(echo "$entry" | awk '{print $1}')
    dst=$(echo "$entry" | awk '{print $2}')
    url="$REPO_BASE/$src"
    info "Downloading $src..."
    if curl -sSfL "$url" -o "$dst"; then
      ok "$(basename "$dst")"
    else
      err "Failed to download $src from $url"
      err "Check your internet connection and the GitHub repository."
      exit 1
    fi
  done

  for entry in "${OPTIONAL_FILES[@]}"; do
    src=$(echo "$entry" | awk '{print $1}')
    dst=$(echo "$entry" | awk '{print $2}')
    url="$REPO_BASE/$src"
    info "Downloading $src (optional)..."
    if curl -sSfL "$url" -o "$dst" 2>/dev/null; then
      ok "$(basename "$dst")"
    else
      warn "$(basename "$dst") not yet available — Espy flash will be skipped."
    fi
  done

  chmod +x "$INSTALL_DIR/setup_wizard.py"
  chmod +x "$INSTALL_DIR/lmoe_proxy.py"
  chmod +x "$INSTALL_DIR/lmoe_sync_server.py"

  ok "All files downloaded to $INSTALL_DIR"
}

# ── Desktop shortcut ───────────────────────────────────────────────────────────
create_shortcuts() {
  step "Creating shortcuts"

  # Desktop .desktop file
  DESKTOP_DIR="$HOME/Desktop"
  if [[ ! -d "$DESKTOP_DIR" ]]; then
    DESKTOP_DIR="$HOME"
  fi

  DESKTOP_FILE="$DESKTOP_DIR/LMoE.desktop"
  cat > "$DESKTOP_FILE" << EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=LMoE
Comment=Last Man on Earth Survival Intelligence System
Exec=chromium-browser --app=file://$INSTALL_DIR/lmoe.html --start-maximized
Icon=utilities-terminal
Terminal=false
Categories=Utility;
EOF
  chmod +x "$DESKTOP_FILE"
  ok "Desktop shortcut: $DESKTOP_FILE"

  # Terminal command: 'lmoe'
  LMOE_CMD="/usr/local/bin/lmoe"
  sudo bash -c "cat > $LMOE_CMD" << 'CMDEOF'
#!/usr/bin/env bash
# LMoE launcher
INSTALL_DIR="$HOME/lmoe"
chromium-browser --app="file://$INSTALL_DIR/lmoe.html" --start-maximized &
CMDEOF
  sudo chmod +x "$LMOE_CMD"
  ok "Terminal command: lmoe"

  # Also create 'lmoe-setup' command
  SETUP_CMD="/usr/local/bin/lmoe-setup"
  PYTHON=$(command -v python3)
  sudo bash -c "cat > $SETUP_CMD" << SETUPEOF
#!/usr/bin/env bash
cd "$INSTALL_DIR" && $PYTHON setup_wizard.py
SETUPEOF
  sudo chmod +x "$SETUP_CMD"
  ok "Setup command: lmoe-setup"
}

# ── Autostart on Pi ────────────────────────────────────────────────────────────
setup_autostart_pi() {
  if ! $IS_PI; then return; fi

  step "Configuring Raspberry Pi autostart"

  AUTOSTART_DIR="$HOME/.config/autostart"
  mkdir -p "$AUTOSTART_DIR"

  cat > "$AUTOSTART_DIR/lmoe.desktop" << EOF
[Desktop Entry]
Type=Application
Name=LMoE
Exec=chromium-browser --app=file://$INSTALL_DIR/lmoe.html --start-maximized --kiosk
EOF
  ok "LMoE will open automatically on login"

  # Make kiosk mode optional — comment it out by default
  sed -i 's/ --kiosk//' "$AUTOSTART_DIR/lmoe.desktop"
  info "(Kiosk mode disabled by default — edit $AUTOSTART_DIR/lmoe.desktop to enable)"
}

# ── Main ────────────────────────────────────────────────────────────────────────
main() {
  banner "LMoE Installer v$LMOE_VERSION"
  echo -e "  Repository : ${CYAN}$REPO_URL${NC}"
  echo -e "  Install to : $INSTALL_DIR"
  echo -e "  Platform   : $(uname -srm)"
  echo

  detect_platform
  check_python || true
  install_packages
  install_python_packages
  download_files
  create_shortcuts
  setup_autostart_pi

  banner "Installation Complete"
  echo -e "  ${GREEN}LMoE has been installed successfully.${NC}\n"
  echo -e "  ${BOLD}Starting setup wizard...${NC}"
  echo -e "  ${DIM}(You can re-run it later with: lmoe-setup)${NC}\n"
  echo -e "$(printf '─%.0s' {1..60})"
  sleep 1

  # Launch setup wizard
  cd "$INSTALL_DIR"
  python3 setup_wizard.py
}

main "$@"
