#!/bin/bash
# PORTMASTER: smg.zip, Super Mario Galaxy.sh
# Super Mario Galaxy (Nebula: el codigo de Petari compilado nativo, graficos OpenGL ES en la Mali-G31) para la
# Anbernic RG35XX H con KNULLI. Necesita gamedata/game.pak hecho de TU disco (RMGE01) con nebula-pak. GPL-3.0.
#   SELECT+START: salir.   Controles: docs/RG35XXH.md ("Super Mario Galaxy").
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/smg"
cd "$GAMEDIR" || exit 1
exec > >(tee "$GAMEDIR/log.txt") 2>&1
chmod +x "$GAMEDIR/nebula"
if [ ! -f "$GAMEDIR/gamedata/game.pak" ]; then
  echo "Falta gamedata/game.pak (hazlo con nebula-pak desde tu disco de Super Mario Galaxy USA)."
  pm_finish; exit 1
fi
mkdir -p "$GAMEDIR/nand" "$GAMEDIR/data" "$GAMEDIR/crashes"

# Ajustes (se pueden cambiar aqui):
#   GALAXY_GLES_PENDING=wait|skip|uber  shaders nuevos: esperar (tirones la 1a vez), saltar el dibujo, o uber
#   NEBULA_PAD_SCHEME=nintendo|xbox      A a la derecha (nintendo) o abajo (xbox)
export NEBULA_DATA_DIR="$GAMEDIR/data"
export GALAXY_CRASH_DUMP_DIR="$GAMEDIR/crashes"
export GALAXY_RUNTIME_SETTINGS_PATH="$GAMEDIR/settings.ini"
export GALAXY_GLES_PENDING="${GALAXY_GLES_PENDING:-wait}"
#   GALAXY_GLES_EFB_DIVISOR=2            resolucion interna 320x264 (mas rapido); 1: 640x528 (la del Wii)
export GALAXY_GLES_EFB_DIVISOR="${GALAXY_GLES_EFB_DIVISOR:-2}"
export NEBULA_PAD_SCHEME="${NEBULA_PAD_SCHEME:-nintendo}"
#   GALAXY_EXPERIMENTAL_NATIVE_4_3=1     el modo 4:3 del propio juego (la pantalla es 4:3); 0: 16:9 con franjas
export GALAXY_EXPERIMENTAL_NATIVE_4_3="${GALAXY_EXPERIMENTAL_NATIVE_4_3:-1}"
# El host SDL activa el aspecto dinamico por defecto, que no se combina con el 4:3 fijo.
if [ "$GALAXY_EXPERIMENTAL_NATIVE_4_3" = "1" ]; then export GALAXY_EXPERIMENTAL_DYNAMIC_ASPECT=0; fi
#   NEBULA_SHOTS=1                       una captura (shots/present_N.ppm) cada 30 s, 10 como mucho (diagnostico)
if [ "${NEBULA_SHOTS:-1}" = "1" ]; then
  mkdir -p "$GAMEDIR/shots"
  export GALAXY_PRESENT_SNAPSHOT_DIR="$GAMEDIR/shots" GALAXY_PRESENT_SNAPSHOT_EVERY=1800 GALAXY_PRESENT_SNAPSHOT_LAST=18000
fi
#   NEBULA_PROFILE=60                    a los 60 s (y 30 y 60 s despues) anota en log.txt donde gastan su tiempo los hilos
export NEBULA_PROFILE="${NEBULA_PROFILE:-60}"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

#   NEBULA_PERF=1                        CPU, GPU y memoria a su frecuencia maxima mientras se juega (0: lo que tenga el sistema)
PERF_SAVED=""
perf_restore() {
  for entry in $PERF_SAVED; do echo "${entry#*=}" > "${entry%%=*}" 2>/dev/null; done
  PERF_SAVED=""
}
if [ "${NEBULA_PERF:-1}" = "1" ]; then
  for gov in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/class/devfreq/*/governor; do
    [ -w "$gov" ] || continue
    dir=$(dirname "$gov")
    case "$gov" in
      /sys/class/devfreq/*) case "$(basename "$dir")" in *gpu*|*mali*|*1800000*|*dmc*) ;; *) continue ;; esac
        avail="$dir/available_governors" ;;
      *) avail="$dir/scaling_available_governors" ;;
    esac
    grep -qw performance "$avail" 2>/dev/null || continue
    old=$(cat "$gov")
    [ "$old" = "performance" ] && continue
    echo performance > "$gov" 2>/dev/null && PERF_SAVED="$PERF_SAVED $gov=$old"
    echo "rendimiento: $gov $old -> performance"
  done
  for f in /sys/class/devfreq/*/cur_freq; do
    case "$f" in *gpu*|*mali*|*1800000*) echo "gpu: $(cat "$f" 2>/dev/null) Hz (max $(cat "$(dirname "$f")/max_freq" 2>/dev/null))" ;; esac
  done
  trap perf_restore EXIT
fi

$GPTOKEYB "nebula" &
pm_platform_helper "$GAMEDIR/nebula"
./nebula --pak "$GAMEDIR/gamedata/game.pak" --nand "$GAMEDIR/nand"
echo "terminado: status $?"
perf_restore
pm_finish
