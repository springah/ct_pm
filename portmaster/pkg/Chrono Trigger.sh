#!/bin/bash
# PORTMASTER: ct_pm.zip, Chrono Trigger.sh
# Chrono Trigger (Android) via the ct_nx loader. User supplies libchrono.so +
# libc++_shared.so + assets/ from their own legally-owned APK (v2.1.5).

# The PortMaster framework files (control.txt / mod_<cfw>.txt) are sourced at
# runtime from the device, not the repo, and define get_controls, $directory,
# $sdl_controllerconfig, $ESUDO, $DEVICE_ARCH, etc. shellcheck can't see them:
# shellcheck disable=SC1090,SC2154

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR="/$directory/ports/ct"
cd "$GAMEDIR" || exit 1

# --- first-run: require the user-supplied game files ---------------------------
missing=""
[ -f "$GAMEDIR/libchrono.so" ]            || missing="$missing libchrono.so"
[ -f "$GAMEDIR/libc++_shared.so" ]        || missing="$missing libc++_shared.so"
[ -f "$GAMEDIR/assets/resources.bin" ]    || missing="$missing assets/"
if [ -n "$missing" ]; then
  clear
  echo ""
  echo "  Chrono Trigger - missing game files:$missing"
  echo ""
  echo "  Copy these from your own Chrono Trigger Android APK (v2.1.5) into:"
  echo "    $GAMEDIR/"
  echo "    - lib/arm64-v8a/libchrono.so      -> libchrono.so"
  echo "    - lib/arm64-v8a/libc++_shared.so  -> libc++_shared.so"
  echo "    - the whole assets/ folder        -> assets/"
  echo ""
  echo "  (closing in 15s)"
  sleep 15
  exit 1
fi

# --- fonts: ship ChronoType (font.ttf); self-heal a CJK fallback for ja/ko/zh --
if [ ! -f "$GAMEDIR/fonts/standard.ttf" ]; then
  mkdir -p "$GAMEDIR/fonts"
  for f in /usr/share/emulationstation/resources/DroidSansFallbackFull.ttf \
           "$controlfolder/resources/NotoSansSC-Regular.ttf" \
           /usr/share/fonts/dejavu/DejaVuSans.ttf; do
    [ -f "$f" ] && cp -f "$f" "$GAMEDIR/fonts/standard.ttf" && break
  done
fi

# --- exports ------------------------------------------------------------------
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"   # framework pad mapping
export GAMEDIR
export CT_FONT_SCALE=1.5   # pixel-font (ChronoType) visual-size scale; tune to taste
# SNES-style text drop-shadow is baked into the binary (force, 1px down-right, 70%).
# To override: export CT_TEXT_SHADOW="off" | "auto" | "dx,dy,opacity"
# (offsets in final pixels, opacity 0..1).

$ESUDO chmod +x "$GAMEDIR/ct" 2>/dev/null

# --- RAM: add compressed swap (zram) for the session --------------------------
# This box is 1 GB with no swap. The engine peaks well past that when a heavy
# scene loads right after the attract FMV (whose decoder heap is still resident),
# and the kernel OOM-kills the game. A ~768 MB zram device gives that spike a
# compressed-RAM overflow to land in instead of dying. Torn down on exit.
ZRAM_DEV=""
$ESUDO modprobe zram >/dev/null 2>&1   # built as a module on this kernel (4.9.x)
if [ -e /sys/class/zram-control/hot_add ] && [ -z "$(cat /proc/swaps | grep zram)" ]; then
  ZN=$(cat /sys/class/zram-control/hot_add 2>/dev/null)
  if [ -n "$ZN" ] && [ -e "/sys/block/zram${ZN}/disksize" ]; then
    echo lz4 | $ESUDO tee "/sys/block/zram${ZN}/comp_algorithm" >/dev/null 2>&1
    echo $((768 * 1024 * 1024)) | $ESUDO tee "/sys/block/zram${ZN}/disksize" >/dev/null 2>&1
    if $ESUDO mkswap "/dev/zram${ZN}" >/dev/null 2>&1 && $ESUDO swapon -p 100 "/dev/zram${ZN}" >/dev/null 2>&1; then
      ZRAM_DEV="$ZN"
    fi
  fi
fi

# --- CPU: balance battery vs smoothness ---------------------------------------
# CT is a light 2D RPG and vsync-capped, so pinning all cores to max frequency
# (performance) for the whole session just burns battery through menus, dialogue
# and vsync idles. Instead use schedutil (scales with load) but raise the min-freq
# FLOOR so a brief idle can't dither the clock down to the bottom and cause the
# micro-stutter the old `performance` pin was guarding against. High clocks only
# when the game actually needs them. Original governor + min-freq restored on exit.
# Tunables: CT_GOV (governor), CT_MIN_KHZ (floor). Set CT_GOV=performance to revert.
PLAY_GOV="${CT_GOV:-schedutil}"
PLAY_MIN_KHZ="${CT_MIN_KHZ:-1008000}"   # keep it from dithering below ~1.0 GHz mid-frame
SAVED_GOV=""
SAVED_MIN=""
if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  SAVED_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
  SAVED_MIN=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null)
  for p in /sys/devices/system/cpu/cpu*/cpufreq; do
    echo "$PLAY_MIN_KHZ" | $ESUDO tee "$p/scaling_min_freq" >/dev/null 2>&1
    echo "$PLAY_GOV"     | $ESUDO tee "$p/scaling_governor" >/dev/null 2>&1
  done
fi

pm_platform_helper "$GAMEDIR/ct"
./ct > "$GAMEDIR/log.txt" 2>&1

if [ -n "$SAVED_GOV" ]; then
  for p in /sys/devices/system/cpu/cpu*/cpufreq; do
    echo "$SAVED_GOV" | $ESUDO tee "$p/scaling_governor" >/dev/null 2>&1
    [ -n "$SAVED_MIN" ] && echo "$SAVED_MIN" | $ESUDO tee "$p/scaling_min_freq" >/dev/null 2>&1
  done
fi

if [ -n "$ZRAM_DEV" ]; then
  $ESUDO swapoff "/dev/zram${ZRAM_DEV}" >/dev/null 2>&1
  echo "$ZRAM_DEV" | $ESUDO tee /sys/class/zram-control/hot_remove >/dev/null 2>&1
fi

printf "\033[H\033[2J"
pm_finish
