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
echo "nebula: $(md5sum "$GAMEDIR/nebula" | cut -c1-12) del $(date -r "$GAMEDIR/nebula" '+%F %T' 2>/dev/null)"
if [ ! -f "$GAMEDIR/gamedata/game.pak" ]; then
  echo "Falta gamedata/game.pak (hazlo con nebula-pak desde tu disco de Super Mario Galaxy USA)."
  pm_finish; exit 1
fi
mkdir -p "$GAMEDIR/nand" "$GAMEDIR/data" "$GAMEDIR/crashes"

# Ajustes (se pueden cambiar aqui):
#   NEBULA_PERFIL=bajo|normal            bajo (por defecto): pocos recursos -- salta frames cuando el render no llega
#                                        (el juego mantiene su velocidad), menos memoria para caches; normal: sin saltos
NEBULA_PERFIL="${NEBULA_PERFIL:-bajo}"
if [ "$NEBULA_PERFIL" = "bajo" ]; then
  #   GALAXY_FRAMESKIP=auto|0|1|2|3      auto: ritmo estable de 60, 30 o 20 imagenes por segundo segun lo que
  #                                      cueste dibujar (GALAXY_FRAMESKIP_MAX=2: 20 como minimo); 1: siempre 30
  export GALAXY_FRAMESKIP="${GALAXY_FRAMESKIP:-auto}"
  export GALAXY_FRAMESKIP_MAX="${GALAXY_FRAMESKIP_MAX:-2}"
  #   GALAXY_PEEK_MODE=async|sync|off    lo que el juego lee de la imagen (destellos del sol, puntero): async usa el
  #                                      frame anterior sin esperar a la GPU; sync espera (exacto, lento); off no lee
  export GALAXY_PEEK_MODE="${GALAXY_PEEK_MODE:-async}"
  export GALAXY_GX_DISPLAY_LIST_CACHE_MB="${GALAXY_GX_DISPLAY_LIST_CACHE_MB:-192}"
  export GALAXY_EFB_COPY_DEST_BUDGET_MB="${GALAXY_EFB_COPY_DEST_BUDGET_MB:-64}"
  export GALAXY_TEXTURE_POOL_MB="${GALAXY_TEXTURE_POOL_MB:-128}"
  export GALAXY_DISC_PREFETCH_BYTES="${GALAXY_DISC_PREFETCH_BYTES:-2097152}"
else
  export GALAXY_FRAMESKIP="${GALAXY_FRAMESKIP:-0}"
fi
echo "perfil: $NEBULA_PERFIL (frameskip $GALAXY_FRAMESKIP, peeks ${GALAXY_PEEK_MODE:-sync})"
#   GALAXY_GLES_PENDING=wait|skip|uber  shaders nuevos: esperar (tirones la 1a vez), saltar el dibujo, o uber
#   NEBULA_PAD_SCHEME=nintendo|xbox      A a la derecha (nintendo) o abajo (xbox)
export NEBULA_DATA_DIR="$GAMEDIR/data"
export GALAXY_CRASH_DUMP_DIR="$GAMEDIR/crashes"
export GALAXY_RUNTIME_SETTINGS_PATH="$GAMEDIR/settings.ini"
export GALAXY_GLES_PENDING="${GALAXY_GLES_PENDING:-wait}"
#   GALAXY_GLES_RESOLUTION=50            resolucion interna en %: 50 = 320x264 (mas rapido), 75 = 480x396, 100 = 640x528
export GALAXY_GLES_RESOLUTION="${GALAXY_GLES_RESOLUTION:-50}"
#   GALAXY_GLES_SHARPEN=0.8              nitidez al ampliar la imagen interna (0: apagada, 0.5 suave, 1.5 fuerte; sin halos)
export GALAXY_GLES_SHARPEN="${GALAXY_GLES_SHARPEN:-0.8}"
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
perf_restore() {  # al reves de como se cambiaron (el minimo vuelve antes que el maximo)
  local list="" entry
  for entry in $PERF_SAVED; do list="$entry $list"; done
  for entry in $list; do echo "${entry#*=}" > "${entry%%=*}" 2>/dev/null; done
  PERF_SAVED=""
}
perf_set() {  # fichero valor: lo escribe y apunta el valor anterior para restaurarlo al salir
  local old; old=$(cat "$1" 2>/dev/null)
  [ "$old" = "$2" ] && return 0
  if echo "$2" > "$1" 2>/dev/null; then PERF_SAVED="$PERF_SAVED $1=$old"; echo "rendimiento: $1 $old -> $2"
  else echo "rendimiento: no se pudo escribir $2 en $1 (sigue en $old)"; fi
}
top_freq() { tr ' ' '\n' < "$1" 2>/dev/null | grep -E '^[0-9]+$' | sort -n | tail -1; }
# Como estan las frecuencias (diagnostico).
for dir in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$dir" ] && echo "cpu $(basename "$dir"): $(cat "$dir/scaling_governor" 2>/dev/null) [$(cat "$dir/scaling_available_governors" 2>/dev/null)] $(cat "$dir/scaling_cur_freq" 2>/dev/null) kHz ($(cat "$dir/scaling_min_freq" 2>/dev/null)-$(cat "$dir/scaling_max_freq" 2>/dev/null), hasta $(cat "$dir/cpuinfo_max_freq" 2>/dev/null))"
done
for dir in /sys/class/devfreq/*; do
  [ -d "$dir" ] && echo "devfreq $(basename "$dir"): $(cat "$dir/governor" 2>/dev/null) [$(cat "$dir/available_governors" 2>/dev/null)] $(cat "$dir/cur_freq" 2>/dev/null) Hz ($(cat "$dir/min_freq" 2>/dev/null)-$(cat "$dir/max_freq" 2>/dev/null); $(cat "$dir/available_frequencies" 2>/dev/null))"
done
if [ "${NEBULA_PERF:-1}" = "1" ]; then
  for dir in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$dir" ] || continue
    if grep -qw performance "$dir/scaling_available_governors" 2>/dev/null; then
      perf_set "$dir/scaling_governor" performance
    else
      top=$(cat "$dir/cpuinfo_max_freq" 2>/dev/null)
      [ -n "$top" ] && perf_set "$dir/scaling_max_freq" "$top" && perf_set "$dir/scaling_min_freq" "$top"
    fi
  done
  for dir in /sys/class/devfreq/*; do
    case "$(basename "$dir")" in *gpu*|*mali*|*1800000*|*dmc*) ;; *) continue ;; esac
    if grep -qw performance "$dir/available_governors" 2>/dev/null; then
      perf_set "$dir/governor" performance
    fi
    # Sin "performance" (o si el driver no le hace caso): el minimo en la frecuencia mas alta.
    top=$(top_freq "$dir/available_frequencies")
    if [ -n "$top" ]; then
      perf_set "$dir/max_freq" "$top"
      perf_set "$dir/min_freq" "$top"
    fi
  done
  trap perf_restore EXIT
fi

$GPTOKEYB "nebula" &
pm_platform_helper "$GAMEDIR/nebula"
# Memoria del juego y libre en el sistema, cada 60 s (diagnostico para el perfil bajo).
( while sleep 60; do
    pid=$(pidof nebula 2>/dev/null | cut -d' ' -f1); [ -n "$pid" ] || continue
    gpu=""; for f in /sys/class/devfreq/*/cur_freq; do case "$f" in *gpu*|*mali*|*1800000*) gpu="$(cat "$f" 2>/dev/null)";; esac; done
    echo "memoria: nebula $(grep VmRSS /proc/$pid/status | tr -s ' ' | cut -d' ' -f2) kB, libre $(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2) kB; cpu $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null) kHz, gpu $gpu Hz"
  done ) &
MEMLOG_PID=$!
./nebula --pak "$GAMEDIR/gamedata/game.pak" --nand "$GAMEDIR/nand"
status=$?
pkill -P "$MEMLOG_PID" 2>/dev/null; kill "$MEMLOG_PID" 2>/dev/null
echo "terminado: status $status"
perf_restore
pm_finish
