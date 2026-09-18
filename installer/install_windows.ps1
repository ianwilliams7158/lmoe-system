#Requires -Version 5.1
<#
.SYNOPSIS
    LMoE — Last Man on Earth — Windows Installer

.DESCRIPTION
    Downloads and installs LMoE on Windows 10 or 11.

    Run from PowerShell (as Administrator recommended):
        Set-ExecutionPolicy -Scope CurrentUser Bypass -Force
        irm https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main/installer/install_windows.ps1 | iex

    Or download and run:
        Save install_windows.ps1 to your Downloads folder, then:
        Right-click → Run with PowerShell

.NOTES
    Requires: Windows 10 1903+ or Windows 11
    Installs to: C:\LMoE  (or %USERPROFILE%\lmoe if no admin rights)
    Python 3.9+ must be installed or will be downloaded automatically.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ── Constants ──────────────────────────────────────────────────────────────────
$REPO_BASE    = 'https://raw.githubusercontent.com/ianwilliams7158/lmoe-system/main'
$REPO_URL     = 'https://github.com/ianwilliams7158/lmoe-system'
$LMOE_VERSION = '6.0'
$PYTHON_URL   = 'https://www.python.org/ftp/python/3.12.4/python-3.12.4-amd64.exe'
$INSTALL_DIR  = Join-Path $env:USERPROFILE 'lmoe'

# Check if we have admin and can use C:\LMoE
$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)
if ($IsAdmin) {
    $INSTALL_DIR = 'C:\LMoE'
}

# ── Colour helpers ─────────────────────────────────────────────────────────────
function Write-Ok   ($msg) { Write-Host "  [OK] $msg"   -ForegroundColor Green  }
function Write-Warn ($msg) { Write-Host "  [!!] $msg"   -ForegroundColor Yellow }
function Write-Err  ($msg) { Write-Host "  [XX] $msg"   -ForegroundColor Red    }
function Write-Info ($msg) { Write-Host "   .   $msg"   -ForegroundColor Gray   }
function Write-Step ($msg) {
    Write-Host ""
    Write-Host "  -- $msg --" -ForegroundColor Cyan
    Write-Host "  $('─' * 56)" -ForegroundColor DarkGray
}
function Write-Banner ($msg) {
    Write-Host ""
    Write-Host ("=" * 60) -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor White
    Write-Host ("=" * 60) -ForegroundColor Cyan
    Write-Host ""
}

# ── Download helper ────────────────────────────────────────────────────────────
function Download-File ($url, $dest, [switch]$Optional) {
    try {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
        Write-Ok (Split-Path $dest -Leaf)
    }
    catch {
        if ($Optional) {
            Write-Warn "$(Split-Path $dest -Leaf) not available yet — skipping."
        }
        else {
            Write-Err "Failed to download $url"
            Write-Err $_.Exception.Message
            throw
        }
    }
}

# ── Python detection / install ─────────────────────────────────────────────────
function Ensure-Python {
    Write-Step "Checking Python"

    # Try common locations
    $candidates = @('python', 'python3', 'py')
    foreach ($cmd in $candidates) {
        try {
            $ver = & $cmd --version 2>&1
            if ($ver -match 'Python (\d+)\.(\d+)') {
                $maj = [int]$Matches[1]; $min = [int]$Matches[2]
                if ($maj -ge 3 -and $min -ge 9) {
                    Write-Ok "Python $($Matches[0].Replace('Python ','')) found at: $(Get-Command $cmd | Select-Object -ExpandProperty Source)"
                    return $cmd
                }
            }
        } catch { }
    }

    Write-Warn "Python 3.9+ not found."
    $ans = Read-Host "  Download and install Python 3.12 now? [Y/n]"
    if ($ans -eq '' -or $ans -match '^[Yy]') {
        Write-Info "Downloading Python 3.12 installer..."
        $pyInstaller = Join-Path $env:TEMP 'python_installer.exe'
        Download-File $PYTHON_URL $pyInstaller
        Write-Info "Running Python installer (this may take a minute)..."
        Write-Info "IMPORTANT: Tick 'Add Python to PATH' on the first screen."
        Start-Process -FilePath $pyInstaller -ArgumentList '/quiet', 'InstallAllUsers=0', 'PrependPath=1', 'Include_test=0' -Wait
        Remove-Item $pyInstaller -Force

        # Refresh PATH
        $env:PATH = [System.Environment]::GetEnvironmentVariable('PATH', 'User') + ';' + `
                    [System.Environment]::GetEnvironmentVariable('PATH', 'Machine')
        Write-Ok "Python installed."
        return 'python'
    }
    else {
        Write-Err "Python is required. Install from https://www.python.org and re-run this installer."
        exit 1
    }
}

# ── pip install ────────────────────────────────────────────────────────────────
function Install-PythonPackages ($python) {
    Write-Step "Installing Python packages"

    $packages = @(
        'libzim',
        'beautifulsoup4',
        'requests',
        'esptool'
    )

    foreach ($pkg in $packages) {
        Write-Info "Installing $pkg..."
        try {
            & $python -m pip install $pkg --quiet 2>&1 | Out-Null
            Write-Ok $pkg
        }
        catch {
            Write-Warn "Could not install $pkg — some features may be limited."
        }
    }
}

# ── Download LMoE files ────────────────────────────────────────────────────────
function Download-LMoEFiles {
    Write-Step "Downloading LMoE files"

    # Create directory structure
    @(
        $INSTALL_DIR,
        "$INSTALL_DIR\espy\firmware",
        "$INSTALL_DIR\data\lmoe",
        "$INSTALL_DIR\data\field\markers",
        "$INSTALL_DIR\data\field\journal",
        "$INSTALL_DIR\www"
    ) | ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }

    # Required files
    $files = @(
        @{ src = 'lmoe/lmoe.html';             dst = "$INSTALL_DIR\lmoe.html" },
        @{ src = 'lmoe/lmoe_config.json';       dst = "$INSTALL_DIR\lmoe_config.json" },
        @{ src = 'lmoe/lmoe_proxy.py';          dst = "$INSTALL_DIR\lmoe_proxy.py" },
        @{ src = 'lmoe/lmoe_sync_server.py';    dst = "$INSTALL_DIR\lmoe_sync_server.py" },
        @{ src = 'lmoe/lmoe_zim_indexer.py';    dst = "$INSTALL_DIR\lmoe_zim_indexer.py" },
        @{ src = 'espy/firmware/main.cpp';       dst = "$INSTALL_DIR\espy\firmware\main.cpp" },
        @{ src = 'espy/firmware/platformio.ini'; dst = "$INSTALL_DIR\espy\firmware\platformio.ini" },
        @{ src = 'installer/setup_wizard.py';    dst = "$INSTALL_DIR\setup_wizard.py" }
    )

    foreach ($f in $files) {
        $url = "$REPO_BASE/$($f.src)"
        Write-Info "Downloading $($f.src)..."
        Download-File $url $f.dst
    }

    # Optional: Espy firmware binary
    $espyBin = "$INSTALL_DIR\espy\firmware\lmoe_espy.bin"
    Write-Info "Downloading Espy firmware (optional)..."
    Download-File "$REPO_BASE/espy/firmware/lmoe_espy.bin" $espyBin -Optional

    Write-Ok "All files downloaded to $INSTALL_DIR"
}

# ── Chrome shortcut ────────────────────────────────────────────────────────────
function Create-Shortcuts ($python) {
    Write-Step "Creating shortcuts"

    # Find Chrome
    $chromePaths = @(
        "${env:ProgramFiles}\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "${env:LocalAppData}\Google\Chrome\Application\chrome.exe"
    )
    $chrome = $null
    foreach ($p in $chromePaths) {
        if (Test-Path $p) { $chrome = $p; break }
    }
    if (-not $chrome) {
        # Try Edge as fallback
        $edgePath = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
        if (Test-Path $edgePath) {
            $chrome = $edgePath
            Write-Warn "Chrome not found — using Microsoft Edge."
        } else {
            Write-Warn "Neither Chrome nor Edge found. Open lmoe.html manually."
        }
    }

    $lmoeFile = "$INSTALL_DIR\lmoe.html"

    # Desktop shortcut
    $desktopPath = [Environment]::GetFolderPath('Desktop')
    $shortcutPath = Join-Path $desktopPath 'LMoE.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($shortcutPath)
    if ($chrome) {
        $shortcut.TargetPath = $chrome
        $shortcut.Arguments  = "--app=`"file:///$($lmoeFile.Replace('\','/'))`" --start-maximized"
    } else {
        $shortcut.TargetPath = $lmoeFile
    }
    $shortcut.WorkingDirectory = $INSTALL_DIR
    $shortcut.Description      = 'Last Man on Earth Survival Intelligence System'
    $shortcut.Save()
    Write-Ok "Desktop shortcut: $shortcutPath"

    # Start Menu shortcut
    $startMenuDir = Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs\LMoE'
    New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null
    $smShortcut = $shell.CreateShortcut("$startMenuDir\LMoE.lnk")
    $smShortcut.TargetPath     = $shortcut.TargetPath
    $smShortcut.Arguments      = $shortcut.Arguments
    $smShortcut.WorkingDirectory = $INSTALL_DIR
    $smShortcut.Description    = 'LMoE Survival Intelligence System'
    $smShortcut.Save()

    # Setup shortcut in Start Menu
    $setupShortcut = $shell.CreateShortcut("$startMenuDir\LMoE Setup.lnk")
    $setupShortcut.TargetPath     = $python
    $setupShortcut.Arguments      = "`"$INSTALL_DIR\setup_wizard.py`""
    $setupShortcut.WorkingDirectory = $INSTALL_DIR
    $setupShortcut.Description    = 'Re-run LMoE Setup Wizard'
    $setupShortcut.Save()

    Write-Ok "Start Menu shortcuts: $startMenuDir"

    # Add install dir to user PATH for convenience
    $userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
    if ($userPath -notlike "*$INSTALL_DIR*") {
        [Environment]::SetEnvironmentVariable('PATH', "$userPath;$INSTALL_DIR", 'User')
        Write-Ok "Added $INSTALL_DIR to user PATH"
    }
}

# ── Windows Defender exclusion ─────────────────────────────────────────────────
function Add-DefenderExclusion {
    if (-not $IsAdmin) { return }
    try {
        Add-MpPreference -ExclusionPath $INSTALL_DIR -ErrorAction SilentlyContinue
        Write-Ok "Windows Defender exclusion added for $INSTALL_DIR"
    } catch {
        Write-Info "Could not add Defender exclusion (non-critical)."
    }
}

# ── Startup tasks for background services ─────────────────────────────────────
function Register-StartupTasks ($python) {
    Write-Step "Registering startup tasks"

    $tasks = @(
        @{ name = 'LMoE-Proxy'; script = "$INSTALL_DIR\lmoe_proxy.py";       desc = 'LMoE OpenSky Proxy' },
        @{ name = 'LMoE-Sync';  script = "$INSTALL_DIR\lmoe_sync_server.py"; desc = 'LMoE Espy Sync Server' }
    )

    foreach ($t in $tasks) {
        try {
            $action  = New-ScheduledTaskAction -Execute $python -Argument "`"$($t.script)`"" -WorkingDirectory $INSTALL_DIR
            $trigger = New-ScheduledTaskTrigger -AtLogOn
            $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit 0 -RestartOnIdle
            Register-ScheduledTask -TaskName "LMoE\$($t.name)" -Action $action `
                -Trigger $trigger -Settings $settings -Description $t.desc `
                -RunLevel Highest -Force | Out-Null
            Write-Ok "$($t.name) registered (starts at login)"

            # Start immediately
            Start-ScheduledTask -TaskName "LMoE\$($t.name)" -ErrorAction SilentlyContinue
        }
        catch {
            Write-Warn "Could not register $($t.name): $($_.Exception.Message)"
            Write-Info "Start manually: $python `"$($t.script)`""
        }
    }
}

# ── Main ───────────────────────────────────────────────────────────────────────
function Main {
    Write-Banner "LMoE Installer v$LMOE_VERSION"
    Write-Host "  Repository : $REPO_URL" -ForegroundColor Cyan
    Write-Host "  Install to : $INSTALL_DIR"
    Write-Host "  Platform   : Windows $([Environment]::OSVersion.Version)"
    Write-Host "  Admin      : $IsAdmin"
    Write-Host ""

    if (-not $IsAdmin) {
        Write-Warn "Not running as Administrator."
        Write-Info "Some features (startup tasks, Defender exclusions) require admin rights."
        Write-Info "To run as admin: right-click PowerShell → Run as Administrator"
        Write-Host ""
        $continue = Read-Host "  Continue without admin rights? [Y/n]"
        if ($continue -match '^[Nn]') { exit 0 }
    }

    $python = Ensure-Python
    Install-PythonPackages $python
    Download-LMoEFiles
    Create-Shortcuts $python
    Add-DefenderExclusion

    if ($IsAdmin) {
        Register-StartupTasks $python
    } else {
        Write-Warn "Startup tasks not registered (need admin). Start services manually if needed:"
        Write-Info "  $python `"$INSTALL_DIR\lmoe_proxy.py`""
        Write-Info "  $python `"$INSTALL_DIR\lmoe_sync_server.py`""
    }

    Write-Banner "Installation Complete"
    Write-Host "  LMoE has been installed to: $INSTALL_DIR" -ForegroundColor Green
    Write-Host ""
    Write-Host "  Starting setup wizard..." -ForegroundColor White
    Write-Host "  (You can re-run it later from Start Menu -> LMoE -> LMoE Setup)" -ForegroundColor Gray
    Write-Host ""
    Start-Sleep -Seconds 1

    # Launch setup wizard
    Set-Location $INSTALL_DIR
    & $python "$INSTALL_DIR\setup_wizard.py"
}

Main
