#!/bin/bash
# PORTMASTER: nebulaprobe.zip, Nebula Probe.sh
# Nebula Probe: mide memoria, CPU y GPU de la consola para un port recompilado (Nebula/NextOS). GPL-3.0.
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
source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/nebulaprobe"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export CFW_NAME DEVICE_NAME
chmod +x "$GAMEDIR/nebula-probe"

$GPTOKEYB "nebula-probe" -c "$GAMEDIR/nebula-probe.gptk" &
pm_platform_helper "$GAMEDIR/nebula-probe"

# If the NextOS Mario Kart Wii port is installed next to this one, its NXCaps memory sampler
# (PSS + swap + GPU memory) watches the probe too; its output is kept as it is.
NXCAPS="$GAMEDIR/../mkwiita-nextos/runtime/nxcaps"
./nebula-probe --out "$GAMEDIR" "$@" &
PROBE_PID=$!
if [ -x "$NXCAPS" ]; then
  "$NXCAPS" memsample --pid "$PROBE_PID" --interval-ms 500 --max-samples 600 --budget-mib 300 \
    > "$GAMEDIR/nxcaps-memsample.json" 2> "$GAMEDIR/nxcaps-memsample.err" &
fi
wait "$PROBE_PID"
STATUS=$?
echo "nebula-probe status=$STATUS (report: $GAMEDIR/nebula-probe-report.txt)"
pm_finish
