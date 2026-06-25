#!/bin/bash
# PORTMASTER: ct.zip, Chrono Trigger.sh
# Chrono Trigger (Android) via the ct_nx loader. User supplies libchrono.so +
# libc++_shared.so + assets/ from their own legally-owned APK (v2.1.5).

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
cd "$GAMEDIR"

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
export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"   # framework pad mapping
export GAMEDIR
export CT_FONT_SCALE=1.5   # pixel-font (ChronoType) visual-size scale; tune to taste

$ESUDO chmod +x "$GAMEDIR/ct" 2>/dev/null

# --- CPU: pin the performance governor during play ----------------------------
# The engine's update+render runs on one thread, so brief vsync idles can let an
# on-demand/schedutil governor dither the clock down and cause micro-stutter.
# Pin performance for the session and restore the original governor on exit.
SAVED_GOV=""
if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
  SAVED_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | $ESUDO tee "$g" >/dev/null 2>&1
  done
fi

./ct > "$GAMEDIR/log.txt" 2>&1

if [ -n "$SAVED_GOV" ]; then
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo "$SAVED_GOV" | $ESUDO tee "$g" >/dev/null 2>&1
  done
fi

printf "\033[H\033[2J"
