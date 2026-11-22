This is screen mirroring tool for EZCast devices. Tested only on EZCast MagicEther. Works via EZCast's own protocol.

Linux-only, X11-only.

```
  ./ezcast.sh [-d] [-f MAX_FPS] [-q QUALITY] [-o VIRTUAL_OUTPUT] <DONGLE_IP> [W H]

Modes:
  1. Mirror primary screen (no W H given):
     ./ezcast.sh [-d] [-f MAX_FPS] [-q QUALITY] <DONGLE_IP>

  2. Create virtual screen WxH to the right of primary and mirror it:
     ./ezcast.sh [-d] [-f MAX_FPS] [-q QUALITY] [-o VIRTUAL_OUTPUT] <DONGLE_IP> <W> <H>

Options:
  -d                 Send only changed frames
  -f MAX_FPS         Max FPS limit (default: 0 = uncapped)
  -q QUALITY         JPEG quality 1-100 (default: 80)
  -o VIRTUAL_OUTPUT  Force specific xrandr output name (e.g., VIRTUAL1, HDMI-2)
```
