#!/bin/bash
# PORTMASTER: nxaccel-mkw.zip, NXAccel Mario Kart Wii.sh
# Instala NXAccel en el port NextOS de Mario Kart Wii (mkwiita-nextos): codigo nativo para la
# decodificacion de texturas GX, Yaz0/Yay0 y DSP-ADPCM que el preparador del aparato hace en Python.
# Solo agrega archivos al Python del preparador; no modifica ninguno del port. GPL-3.0.
# Para desinstalar: crear el archivo nxaccel-mkw/UNINSTALL y volver a ejecutar.
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"

SELF=$(readlink -f "$0"); PORTS=${SELF%/*}
[ -d "$PORTS/nxaccel-mkw" ] || PORTS="/$directory/ports"
GAMEDIR="$PORTS/nxaccel-mkw"
MKW="$PORTS/mkwiita-nextos"
SDK="$MKW/runtime/preparers/sdk/python"
SITE="$SDK/lib/python3.12/site-packages"
exec > >(tee "$GAMEDIR/log.txt") 2>&1
echo "NXAccel 0.1.0 para $MKW"
if [ ! -d "$SITE" ]; then
  echo "No se encontro el port NextOS de Mario Kart Wii (mkwiita-nextos) junto a este."
  [ -n "$controlfolder" ] && command -v pm_finish >/dev/null 2>&1 && pm_finish
  exit 1
fi
uninstall() { rm -rf "$SITE/nxaccel" "$SITE/pyfastyaz0yay0.py" "$SITE/nxaccel.pth"; echo "NXAccel desinstalado."; }
if [ -f "$GAMEDIR/UNINSTALL" ]; then uninstall; rm -f "$GAMEDIR/UNINSTALL"; exit 0; fi

cp -r "$GAMEDIR/python/nxaccel" "$SITE/" && cp "$GAMEDIR/python/pyfastyaz0yay0.py" "$GAMEDIR/python/nxaccel.pth" "$SITE/" || { echo "copia fallida"; exit 1; }
chmod +x "$SDK/bin/python3.12" "$SDK/bin/nx-python3.12" 2>/dev/null

echo "Autoprueba con el Python del propio port (igualdad byte a byte y tiempos en este aparato)..."
PYTHONHOME="$SDK" LD_LIBRARY_PATH="$MKW/runtime/lib:$SDK/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$SDK/bin/python3.12" "$GAMEDIR/tests/test_equivalence.py" \
  --gclib "$MKW/runtime/preparers/mkw/vendor/gclib" --tools "$MKW/runtime/preparers/mkw/tools" \
  --nxaccel "$SITE" --lib "$SITE/nxaccel/libnxaccel.so"
STATUS=$?
if [ $STATUS -ne 0 ]; then
  echo "La autoprueba fallo (status $STATUS): se retira NXAccel; el preparador queda como estaba."
  uninstall
else
  echo "NXAccel instalado. Abra Mario Kart Wii para preparar los datos como siempre."
  echo "Estadisticas de uso: exporte NXACCEL_STATS=<archivo> antes de preparar (ver README)."
fi
command -v pm_finish >/dev/null 2>&1 && pm_finish
exit $STATUS
