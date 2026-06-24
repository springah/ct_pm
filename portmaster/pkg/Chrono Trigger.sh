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

export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"   # framework pad mapping
export GAMEDIR
export CT_FONT_SCALE=1.5   # pixel-font (ChronoType) visual-size scale; tune to taste

cd "$GAMEDIR"
$ESUDO chmod +x "$GAMEDIR/ct" 2>/dev/null

# Native SDL controller (the loader opens the pad itself, with a built-in
# fallback mapping if the framework GUID doesn't match). Plain log redirect.
./ct > "$GAMEDIR/log.txt" 2>&1

printf "\033[H\033[2J"
