#!/bin/bash
# PORTMASTER: smg.zip, Super Mario Galaxy.sh
# Super Mario Galaxy (Nebula: el codigo de Petari compilado nativo, graficos OpenGL ES en la Mali-G31) para las
# Anbernic RG con Allwinner H700 (KNULLI, muOS) y la R36S con Rockchip RK3326 (ArkOS, dArkOS). El juego sale de TU
# disco (RMGE01): Cactus Galaxy Wrapper lo convierte la primera vez. GPL-3.0.
#   Salir: MENU + START (KNULLI, muOS) o SELECT + START (ArkOS, dArkOS).   Controles: smg/README.md.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

# El perfil de controles segun el firmware y la consola (PortMaster da CFW_NAME, DEVICE_NAME y ANALOG_STICKS):
#   KNULLI y muOS (Anbernic RG con Allwinner H700): salir con MENU + START; ArkOS y dArkOS (R36S, Rockchip RK3326):
#   SELECT + START; ROCKNIX, AmberELEC y el resto: cualquiera de los dos. Sin sticks (RG35XX Plus/SP, RG28XX,
#   RG34XX...) la cruceta mueve a Mario; con uno (RG40XX V, RG CubeXX), la cruceta mueve el puntero.
#   NEBULA_FACE_LAYOUT=positions|labels  labels si en tu firmware A y B salen al reves (A deberia ser el de la derecha)
#   NEBULA_STICKS=2|1|0  NEBULA_QUIT_COMBO=menu|select|both  fuerzan el resto
cfw=$(printf '%s' "${CFW_NAME:-}" | tr '[:upper:]' '[:lower:]')
soc=$(tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null | grep -Eo 'h700|h616|rk3326|rk3566' | head -1)
case "$cfw" in
  *knulli*|*muos*) quit_combo=menu ;;
  *arkos*|*darkos*) quit_combo=select ;;
  *) quit_combo=both ;;
esac
export NEBULA_QUIT_COMBO="${NEBULA_QUIT_COMBO:-$quit_combo}"
case "${ANALOG_STICKS:-}" in 0|1|2) sticks=$ANALOG_STICKS ;; *) sticks=2 ;; esac
export NEBULA_STICKS="${NEBULA_STICKS:-$sticks}"
export NEBULA_FACE_LAYOUT="${NEBULA_FACE_LAYOUT:-positions}"
# La base de mandos de PortMaster, por si el firmware no trae el mapeo de este mando.
[ -f "$controlfolder/gamecontrollerdb.txt" ] && export NEBULA_GAMECONTROLLERDB="$controlfolder/gamecontrollerdb.txt"
GAMEDIR="/$directory/ports/smg"
cd "$GAMEDIR" || exit 1
exec > >(tee "$GAMEDIR/log.txt") 2>&1
$ESUDO chmod +x "$GAMEDIR/nebula" 2>/dev/null || chmod +x "$GAMEDIR/nebula"
echo "firmware: ${CFW_NAME:-?} | consola: ${DEVICE_NAME:-?} (${soc:-soc ?}) | sticks: $NEBULA_STICKS | salir: $NEBULA_QUIT_COMBO + START | botones: $NEBULA_FACE_LAYOUT"
echo "nebula: $(md5sum "$GAMEDIR/nebula" | cut -c1-12) del $(date -r "$GAMEDIR/nebula" '+%F %T' 2>/dev/null)"
# Cactus Galaxy Wrapper (smg/cactus-galaxy-wrapper, dentro del port) se abre la primera vez: sin game.pak pide el
# disco (ISO, RVZ, WIA, WBFS, GCZ o CISO en smg/gamedata/ o roms/wii/; lo detecta en cuanto se copia), pregunta el
# idioma y lo convierte con smg/nebula-pak (nativo: sin Termux ni otro equipo); con game.pak, ofrece los idiomas del
# juego. La primera vez, antes de jugar, el cactus con lentes saluda en el idioma elegido. Despues arranca el juego
# directamente; manteniendo SELECT (o L1) al abrirlo vuelven las opciones.
# NEBULA_PAK_DELETE_IMAGE=1 borra la imagen al terminar de convertir.
screen_message() {
  echo "$1"
  if type pm_message >/dev/null 2>&1; then pm_message "$1"; else
    for tty in /dev/tty0 /dev/tty1; do [ -w "$tty" ] && printf '\033c%s\n' "$1" > "$tty" 2>/dev/null; done
  fi
}
for tool in nebula-pak cactus-galaxy-wrapper; do [ -f "$GAMEDIR/$tool" ] && chmod +x "$GAMEDIR/$tool"; done
# Instalaciones anteriores con el juego ya hecho: el saludo de la primera vez (smg/.primer-arranque) no se repite.
[ -f "$GAMEDIR/gamedata/game.pak" ] && [ -f "$GAMEDIR/idioma.txt" ] && [ ! -f "$GAMEDIR/.primer-arranque" ] &&
  echo 1 > "$GAMEDIR/.primer-arranque"
# El wrapper ya no tiene entrada propia en Ports: se quita la de versiones anteriores.
rm -f "/$directory/ports/Cactus Galaxy Wrapper.sh" "$GAMEDIR/cactus.png"
cactus=0
if [ -x "$GAMEDIR/cactus-galaxy-wrapper" ]; then
  wrapper=0
  if [ ! -f "$GAMEDIR/gamedata/game.pak" ] || [ ! -f "$GAMEDIR/idioma.txt" ]; then
    wrapper=1  # primera vez
  elif SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig" "$GAMEDIR/cactus-galaxy-wrapper" --held; then
    wrapper=1  # SELECT mantenido al abrir
  fi
  if [ "$wrapper" = 1 ]; then
    delete=""; [ "${NEBULA_PAK_DELETE_IMAGE:-0}" = "1" ] && delete="--delete-image"
    SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig" "$GAMEDIR/cactus-galaxy-wrapper" --smg "$GAMEDIR" --setup $delete
    cactus=$?
    echo "Cactus Galaxy Wrapper: salida $cactus"
    if [ "$cactus" = 1 ]; then  # salir sin jugar
      pm_finish; exit 0
    fi
  fi
fi
# Sin pantalla para la interfaz (salida 3) o sin ella: la version de texto, con el avance en la consola.
if [ ! -f "$GAMEDIR/gamedata/game.pak" ] && { [ "$cactus" = 3 ] || [ ! -x "$GAMEDIR/cactus-galaxy-wrapper" ]; } && [ -x "$GAMEDIR/nebula-pak" ] && [ -f "$GAMEDIR/cactus-galaxy-wrapper.sh" ]; then
  image=$(find "$GAMEDIR/gamedata" -maxdepth 1 -type f \( -iname '*.iso' -o -iname '*.rvz' -o -iname '*.wia' \
    -o -iname '*.wbfs' -o -iname '*.gcz' -o -iname '*.ciso' \) 2>/dev/null | head -1)
  if [ -n "$image" ]; then
    name=$(basename "$image")
    screen_message "Cactus Galaxy Wrapper: preparando game.pak desde $name (solo la primera vez, unos minutos)..."
    shown=-1
    rm -f /tmp/smg-pak-error
    bash "$GAMEDIR/cactus-galaxy-wrapper.sh" "$image" "$GAMEDIR" 2>&1 | while IFS= read -r line; do
      echo "$line"
      case "$line" in
        *%*MiB*)  # "37% (1234 / 3300 MiB)": la pantalla cada 2 %
          percent=${line%%%*}; percent=${percent// /}
          case "$percent" in ''|*[!0-9]*) continue ;; esac
          if [ $((percent - shown)) -ge 2 ] || [ "$percent" = 100 ]; then
            shown=$percent
            screen_message "Cactus Galaxy Wrapper: preparando game.pak desde $name
$line
(solo la primera vez; no apagues la consola)"
          fi ;;
        ERROR*|Error*) echo "$line" > /tmp/smg-pak-error ;;
      esac
    done
    if [ "${PIPESTATUS[0]}" = 0 ] && [ -f "$GAMEDIR/gamedata/game.pak" ]; then
      screen_message "game.pak listo."
      [ "${NEBULA_PAK_DELETE_IMAGE:-0}" = "1" ] && rm -f "$image"
    else
      screen_message "No se pudo hacer game.pak: $(cat /tmp/smg-pak-error 2>/dev/null || echo 'mira smg/log.txt')"
      sleep 8
    fi
  fi
fi
if [ ! -f "$GAMEDIR/gamedata/game.pak" ]; then
  screen_message "Falta smg/gamedata/game.pak: pon ahi (o en roms/wii) tu disco de Super Mario Galaxy USA (ISO o RVZ) o el game.pak."
  sleep 5
  pm_finish; exit 1
fi
mkdir -p "$GAMEDIR/nand" "$GAMEDIR/data" "$GAMEDIR/crashes"
# Portada y ficha en Ports (la primera vez con red; luego no hace nada): en segundo plano, el juego no espera.
if command -v python3 >/dev/null 2>&1 && [ -f "$GAMEDIR/gamelist.py" ]; then
  python3 "$GAMEDIR/gamelist.py" "$GAMEDIR" &
fi

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
  #   GALAXY_GLES_PEEK_FIELD=25          esas lecturas a 160x132 (25 %): se copian de la GPU 20 veces menos bytes
  export GALAXY_GLES_PEEK_FIELD="${GALAXY_GLES_PEEK_FIELD:-25}"
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
#   GALAXY_GLES_RESOLUTION=100           resolucion interna: 100 = 640x528 (alta definicion, la del Wii); 75 = 480x396;
#                                        50 = 320x264; auto = dinamica (baja cuando la GPU no llega, sube cuando le sobra)
#   GALAXY_GLES_RESOLUTION_MIN/MAX/START  los limites de la dinamica (50/100) y donde empieza (75)
export GALAXY_GLES_RESOLUTION="${GALAXY_GLES_RESOLUTION:-100}"
export GALAXY_GLES_RESOLUTION_MIN="${GALAXY_GLES_RESOLUTION_MIN:-50}"
export GALAXY_GLES_RESOLUTION_MAX="${GALAXY_GLES_RESOLUTION_MAX:-100}"
export GALAXY_GLES_RESOLUTION_START="${GALAXY_GLES_RESOLUTION_START:-75}"
#   GALAXY_GLES_SHARPEN=0.4              nitidez al ampliar la imagen interna (0.25-0.5 recomendado; 0: apagada; sin halos)
export GALAXY_GLES_SHARPEN="${GALAXY_GLES_SHARPEN:-0.4}"
#   GALAXY_GLES_SHADOWS=0.45             aclara las sombras (0.3-0.62 recomendado; 0: apagado); GALAXY_GLES_CONTRAST=0
export GALAXY_GLES_SHADOWS="${GALAXY_GLES_SHADOWS:-0.45}"
export GALAXY_GLES_CONTRAST="${GALAXY_GLES_CONTRAST:-0}"
#   GALAXY_NO_DOF=1 GALAXY_NO_BLUR=1     sin profundidad de campo ni desenfoques (de zona y de movimiento) del juego
export GALAXY_NO_DOF="${GALAXY_NO_DOF:-1}"
export GALAXY_NO_BLUR="${GALAXY_NO_BLUR:-1}"
#   GALAXY_GLES_AA=1                     suaviza los bordes dentados (antialiasing tipo FXAA en la misma copia); 0: apagado
export GALAXY_GLES_AA="${GALAXY_GLES_AA:-1}"
export NEBULA_PAD_SCHEME="${NEBULA_PAD_SCHEME:-nintendo}"
#   GALAXY_LANGUAGE=spanish|english|french  idioma del juego (textos y menus; el disco USA trae los tres). Sin fijarlo
#                                         aqui vale el que se eligio al instalar con Cactus Galaxy Wrapper
# El idioma elegido en Cactus Galaxy Wrapper (smg/idioma.txt; SELECT mantenido al abrir lo cambia), si no se fija aqui.
if [ -z "${GALAXY_LANGUAGE:-}" ] && [ -f "$GAMEDIR/idioma.txt" ]; then
  # "english" o "english ptbr" (el portugues del parche PT-BR va en ese idioma del juego)
  read -r GALAXY_LANGUAGE game_variant < "$GAMEDIR/idioma.txt"
  [ "${game_variant:-}" = ptbr ] && echo "idioma del juego: portugues de Brasil (parche PT-BR)"
fi
case "${GALAXY_LANGUAGE:-}" in spanish|english|french) ;; *) GALAXY_LANGUAGE=spanish ;; esac
export GALAXY_LANGUAGE
echo "idioma del juego: $GALAXY_LANGUAGE"
#   GALAXY_EXPERIMENTAL_NATIVE_4_3=1     el modo 4:3 del propio juego (la pantalla es 4:3); 0: 16:9 con franjas
export GALAXY_EXPERIMENTAL_NATIVE_4_3="${GALAXY_EXPERIMENTAL_NATIVE_4_3:-1}"
# El host SDL activa el aspecto dinamico por defecto, que no se combina con el 4:3 fijo.
if [ "$GALAXY_EXPERIMENTAL_NATIVE_4_3" = "1" ]; then export GALAXY_EXPERIMENTAL_DYNAMIC_ASPECT=0; fi
#   NEBULA_SHOTS=1                       una captura (shots/present_N.ppm) cada 30 s, 10 como mucho (diagnostico)
if [ "${NEBULA_SHOTS:-1}" = "1" ]; then
  mkdir -p "$GAMEDIR/shots"
  export GALAXY_PRESENT_SNAPSHOT_DIR="$GAMEDIR/shots" GALAXY_PRESENT_SNAPSHOT_EVERY=1800 GALAXY_PRESENT_SNAPSHOT_LAST=18000
fi
#   NEBULA_PROFILE=150                   a los 150 s (y 30 y 60 s despues) anota en log.txt donde gastan su tiempo los hilos
export NEBULA_PROFILE="${NEBULA_PROFILE:-150}"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

#   NEBULA_PERF=1                        CPU, GPU y memoria a su frecuencia maxima mientras se juega (0: lo que tenga el sistema)
PERF_SAVED=""
perf_restore() {  # al reves de como se cambiaron (el minimo vuelve antes que el maximo)
  local list="" entry
  for entry in $PERF_SAVED; do list="$entry $list"; done
  for entry in $list; do
    echo "${entry#*=}" > "${entry%%=*}" 2>/dev/null || { [ -n "${ESUDO:-}" ] && echo "${entry#*=}" | $ESUDO tee "${entry%%=*}" >/dev/null 2>&1; }
  done
  PERF_SAVED=""
}
perf_set() {  # fichero valor: lo escribe y apunta el valor anterior para restaurarlo al salir
  local old; old=$(cat "$1" 2>/dev/null)
  [ "$old" = "$2" ] && return 0
  if echo "$2" > "$1" 2>/dev/null || { [ -n "${ESUDO:-}" ] && echo "$2" | $ESUDO tee "$1" >/dev/null 2>&1; }; then
    PERF_SAVED="$PERF_SAVED $1=$old"; echo "rendimiento: $1 $old -> $2"
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

# Salir: la combinacion del firmware con Start (el juego la atiende; gptokeyb mata con la misma si no contesta):
# MENU (HOTKEY=guide) en KNULLI y muOS; SELECT (el de gptokeyb sin HOTKEY) en ArkOS, dArkOS y el resto.
if [ "$NEBULA_QUIT_COMBO" = menu ]; then export HOTKEY=guide; else unset HOTKEY; fi
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
