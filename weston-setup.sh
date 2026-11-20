#!/usr/bin/env bash
# Install a lightweight application set for an existing Weston desktop.
# Does not replace Weston, change the kernel, alter Mali/FakeDRM, or switch network managers.

set -Eeuo pipefail

CONFIG="/etc/mali-wl/weston.ini"
ICON_BASE="/usr/share/icons/HighContrast/22x22"
TARGET_USER="${SUDO_USER:-${USER:-root}}"

if [[ ${EUID} -ne 0 ]]; then
    echo "Run this script with sudo: sudo bash $0" >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This installer requires an apt-based Debian system." >&2
    exit 1
fi

if ! id "$TARGET_USER" >/dev/null 2>&1; then
    TARGET_USER="root"
fi

TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
[[ -n "$TARGET_HOME" && -d "$TARGET_HOME" ]] || TARGET_HOME="/root"

echo "==> Installing lightweight desktop applications"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
    thunar \
    xfce4-appfinder \
    xfce4-settings \
    xfce4-taskmanager \
    mousepad \
    ristretto \
    galculator \
    mpv \
    pavucontrol \
    network-manager-gnome \
    adwaita-icon-theme \
    hicolor-icon-theme

echo
echo "==> Checking existing network manager"
NM_ACTIVE=0
if command -v systemctl >/dev/null 2>&1 &&
   systemctl is-active --quiet NetworkManager; then
    NM_ACTIVE=1
    echo "NetworkManager is active."
else
    echo "NetworkManager is not active. No network service will be enabled or switched."
    echo "The Network Settings launcher will still be installed if nm-connection-editor exists."
fi

echo
echo "==> Checking Weston configuration and icons"
if [[ ! -d /etc/mali-wl ]]; then
    echo "Expected OEM configuration directory /etc/mali-wl does not exist." >&2
    echo "No Weston configuration was changed." >&2
    exit 1
fi

mkdir -p "$(dirname "$CONFIG")"
if [[ -f "$CONFIG" ]]; then
    BACKUP="${CONFIG}.backup.$(date +%Y%m%d-%H%M%S)"
    cp -a "$CONFIG" "$BACKUP"
    echo "Backed up existing configuration to: $BACKUP"
else
    echo "No existing $CONFIG found; creating it."
fi

TMP_CONFIG="$(mktemp)"
trap 'rm -f "$TMP_CONFIG"' EXIT

cat > "$TMP_CONFIG" <<'INI'
[core]
idle-time=0

[shell]
panel-position=top
background-image=/usr/share/weston/background.png
background-type=scale

INI

add_launcher() {
    local icon="$1"
    local command_name="$2"
    local display_name="$3"
    local executable

    executable="$(command -v "$command_name" 2>/dev/null || true)"
    if [[ -z "$executable" ]]; then
        echo "Skipping $display_name: executable '$command_name' not found."
        return
    fi
    if [[ ! -f "$icon" ]]; then
        echo "Skipping $display_name: icon not found: $icon"
        return
    fi

    cat >> "$TMP_CONFIG" <<INI
[launcher]
icon=$icon
path=$executable
displayname=$display_name

INI
    echo "Added launcher: $display_name -> $executable"
}

add_launcher "$ICON_BASE/categories/applications-utilities.png" \
    xfce4-appfinder "Applications"
add_launcher "$ICON_BASE/apps/system-file-manager.png" \
    thunar "Files"
add_launcher "$ICON_BASE/apps/text-editor.png" \
    mousepad "Text Editor"
add_launcher "$ICON_BASE/mimetypes/image-x-generic.png" \
    ristretto "Images"
add_launcher "$ICON_BASE/apps/utilities-system-monitor.png" \
    xfce4-taskmanager "Task Manager"
add_launcher "$ICON_BASE/categories/preferences-system.png" \
    xfce4-settings-manager "Settings"
add_launcher "$ICON_BASE/apps/preferences-system-network.png" \
    nm-connection-editor "Network"
add_launcher "$ICON_BASE/apps/accessories-calculator.png" \
    galculator "Calculator"
add_launcher "$ICON_BASE/apps/utilities-terminal.png" \
    weston-terminal "Terminal"

install -o root -g root -m 0644 "$TMP_CONFIG" "$CONFIG"
echo "Installed Weston configuration: $CONFIG"

echo
echo "==> Configuring NetworkManager tray applet autostart"
if [[ "$NM_ACTIVE" -eq 1 ]] && command -v nm-applet >/dev/null 2>&1; then
    AUTOSTART_DIR="$TARGET_HOME/.config/autostart"
    mkdir -p "$AUTOSTART_DIR"
    cat > "$AUTOSTART_DIR/nm-applet.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=NetworkManager Applet
Comment=Network connection status and controls
Exec=nm-applet --indicator
Terminal=false
X-GNOME-Autostart-enabled=true
DESKTOP
    chown "$TARGET_USER":"$(id -gn "$TARGET_USER")" "$AUTOSTART_DIR" "$AUTOSTART_DIR/nm-applet.desktop"
    echo "Created $AUTOSTART_DIR/nm-applet.desktop"
    echo "Note: Weston desktop-shell may not provide a system tray host, so the icon may not be visible."
else
    echo "Not enabling nm-applet: NetworkManager is not active or nm-applet is unavailable."
    echo "No networking service was enabled, disabled, or replaced."
fi

echo
echo "==> Done"
echo "Weston config: $CONFIG"
echo "Existing Weston/Mali/FakeDRM and boot configuration were not modified."
echo "Restart Weston using your existing OEM startup/service mechanism."
echo "Do not use 'pkill weston' on this device."
