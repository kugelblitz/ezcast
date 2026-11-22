#!/usr/bin/env bash

# if you see "xrandr: Configure crtc 1 failed", do yourself a virtual DVI:
#   sudo apt install evdi-dkms
#   echo "evdi" | sudo tee /etc/modules-load.d/evdi.conf
#   echo "options evdi initial_device_count=1" | sudo tee /etc/modprobe.d/evdi.conf
# and then specify this virtual DVI output as something like "-o DVI-I-1-1" 

set -euo pipefail

MAX_FPS=0
QUALITY=80
VIRT_OUT=""
DIFF_ONLY=0

usage() {
    cat <<EOF
Usage:
  $0 [-d] [-f MAX_FPS] [-q QUALITY] [-o VIRTUAL_OUTPUT] <DONGLE_IP> [W H]

Modes:
  1. Mirror primary screen (no W H given):
     $0 [-d] [-f MAX_FPS] [-q QUALITY] <DONGLE_IP>

  2. Create virtual screen WxH to the right of primary and mirror it:
     $0 [-d] [-f MAX_FPS] [-q QUALITY] [-o VIRTUAL_OUTPUT] <DONGLE_IP> <W> <H>

Options:
  -d                 Send only changed frames
  -f MAX_FPS         Max FPS limit (default: 0 = uncapped)
  -q QUALITY         JPEG quality 1-100 (default: 80)
  -o VIRTUAL_OUTPUT  Force specific xrandr output name (e.g., VIRTUAL1, HDMI-2)
EOF
    exit 1
}

while getopts ":df:q:o:h" opt; do
    case "$opt" in
        d) DIFF_ONLY=1 ;;
        f) MAX_FPS="$OPTARG" ;;
        q) QUALITY="$OPTARG" ;;
        o) VIRT_OUT="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

if [ $# -ne 1 ] && [ $# -ne 3 ]; then
    usage
fi

DONGLE_IP="$1"
VIRT_W="${2:-}"
VIRT_H="${3:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/ezcast_mirror"

# Build optional binary flags
BIN_FLAGS=(-f "$MAX_FPS" -q "$QUALITY")
if [ "$DIFF_ONLY" -eq 1 ]; then
    BIN_FLAGS+=(-d)
fi

# Auto-compile C binary if missing or older than source
if [ ! -x "$BIN" ]; then
    echo "[Error] $BIN not found." >&2
    exit 1
fi

# Detect primary monitor name, geometry, and current mode from xrandr
XRANDR_OUT="$(xrandr --query)"
PRIM_LINE="$(echo "$XRANDR_OUT" | grep -E ' connected primary [0-9]+x[0-9]+\+[0-9]+\+[0-9]+' | head -n1 || true)"
if [ -z "$PRIM_LINE" ]; then
    PRIM_LINE="$(echo "$XRANDR_OUT" | grep -E ' connected [0-9]+x[0-9]+\+[0-9]+\+[0-9]+' | head -n1 || true)"
fi

if [ -z "$PRIM_LINE" ]; then
    echo "[Error] Could not detect an active primary display via xrandr." >&2
    exit 1
fi

PRIM_OUT="$(echo "$PRIM_LINE" | awk '{print $1}')"
PRIM_GEOM="$(echo "$PRIM_LINE" | grep -oE '[0-9]+x[0-9]+\+[0-9]+\+[0-9]+' | head -n1)"
PRIM_W="${PRIM_GEOM%%x*}"
REST="${PRIM_GEOM#*x}"
PRIM_H="${REST%%+*}"
REST="${REST#*+}"
PRIM_X="${REST%%+*}"
PRIM_Y="${REST#*+}"

# Mode 1: Mirror primary screen directly (1:1)
if [ -z "$VIRT_W" ]; then
    echo "[Layout] Mirroring primary display $PRIM_OUT (${PRIM_W}x${PRIM_H} at +${PRIM_X}+${PRIM_Y})"
    exec "$BIN" "${BIN_FLAGS[@]}" "$DONGLE_IP" "$PRIM_X" "$PRIM_Y" "$PRIM_W" "$PRIM_H"
fi

# Mode 2: Create WxH virtual screen to the right of primary display
if [ -z "$VIRT_OUT" ]; then
    VIRT_OUT="$(echo "$XRANDR_OUT" | awk '/^VIRTUAL[0-9]+ disconnected/ {print $1; exit}')"
fi
if [ -z "$VIRT_OUT" ]; then
    VIRT_OUT="$(echo "$XRANDR_OUT" | awk '/ disconnected/ {print $1; exit}')"
fi
if [ -z "$VIRT_OUT" ]; then
    echo "[Error] No disconnected/VIRTUAL xrandr output found." >&2
    echo "        On Intel Xorg, add Option \"VirtualHeads\" \"1\" in /etc/X11/xorg.conf.d/20-intel.conf." >&2
    exit 1
fi

MODE_NAME="${VIRT_W}x${VIRT_H}_60.00"
MODELINE="$(cvt "$VIRT_W" "$VIRT_H" 60 | awk '/Modeline/ {$1=""; $2=""; print}' | sed 's/^ *//')"

CAP_X=$((PRIM_X + PRIM_W))
CAP_Y="$PRIM_Y"
ROOT_W=$((CAP_X + VIRT_W))
ROOT_H=$((PRIM_H > VIRT_H ? PRIM_H : VIRT_H))
if [ $((CAP_Y + VIRT_H)) -gt "$ROOT_H" ]; then
    ROOT_H=$((CAP_Y + VIRT_H))
fi

cleanup() {
    echo ""
    echo "[Cleanup] Disabling $VIRT_OUT and restoring root framebuffer to ${PRIM_W}x${PRIM_H}..."
    xrandr --output "$VIRT_OUT" --off >/dev/null 2>&1 || true
    xrandr --delmode "$VIRT_OUT" "$MODE_NAME" >/dev/null 2>&1 || true
    xrandr --rmmode "$MODE_NAME" >/dev/null 2>&1 || true
    xrandr --fb "${PRIM_W}x${PRIM_H}" \
           --output "$PRIM_OUT" --mode "${PRIM_W}x${PRIM_H}" --pos "${PRIM_X}x${PRIM_Y}" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

echo "[Layout] Primary: $PRIM_OUT (${PRIM_W}x${PRIM_H} at +${PRIM_X}+${PRIM_Y})"
echo "[Layout] Virtual: $VIRT_OUT (${VIRT_W}x${VIRT_H} at +${CAP_X}+${CAP_Y}), Root FB: ${ROOT_W}x${ROOT_H}"

xrandr --delmode "$VIRT_OUT" "$MODE_NAME" >/dev/null 2>&1 || true
xrandr --rmmode "$MODE_NAME" >/dev/null 2>&1 || true

xrandr --newmode "$MODE_NAME" $MODELINE
xrandr --addmode "$VIRT_OUT" "$MODE_NAME"

xrandr --fb "${ROOT_W}x${ROOT_H}" \
       --output "$PRIM_OUT" --mode "${PRIM_W}x${PRIM_H}" --pos "${PRIM_X}x${PRIM_Y}" \
       --output "$VIRT_OUT" --mode "$MODE_NAME" --pos "${CAP_X}x${CAP_Y}"

"$BIN" "${BIN_FLAGS[@]}" "$DONGLE_IP" "$CAP_X" "$CAP_Y" "$VIRT_W" "$VIRT_H"
