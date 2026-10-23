# Cactus Galaxy Wrapper

La interfaz de la consola (RG35XX H, KNULLI) que convierte el disco de Super Mario Galaxy del jugador en el
`game.pak` del port: un desierto de noche bajo una galaxia, con un saguaro que crece con el avance y florece al
terminar. La conversión la hace `nebula-pak` (`platform/knulli/pak`) como proceso aparte.

- En Ports: **Cactus Galaxy Wrapper** (`platform/knulli/smg/Cactus Galaxy Wrapper.sh`). El lanzador del juego la abre
  sola (`--auto`) si falta `game.pak`; con un único disco válido, convierte sin preguntar y deja arrancar el juego.
- Busca discos en `smg/gamedata/`, `smg/` y `roms/wii/`; los comprueba con `nebula-pak --identify`.
- Idioma del juego: al convertir pregunta Español, English o Français (lo que trae el disco USA) y lo guarda en
  `smg/idioma.txt`, que el lanzador del juego pasa como `GALAXY_LANGUAGE`. En el menú, ◄► lo cambia en cualquier
  momento, también con el `game.pak` ya hecho.
- Português do Brasil: con un .xdelta de la traducción en `smg/patches/` (nombre con PT-BR/ptbr/portugues), el
  idioma "Português" aparece al instalar; elegido, `nebula-pak --patch` aplica el parche al disco mientras hace
  game.pak (ISO, RVZ, WIA, WBFS, GCZ o CISO; sin ISO intermedio). nebula-pak dice qué idioma del juego sustituye la
  traducción (`PATCH_LANGUAGE`: los ficheros cambiados en UsEnglish/UsSpanish/UsFrench); `smg/idioma.txt` queda
  como `english ptbr` y `gamedata/game.pak.idioma` como `ptbr english`. En el menú, ◄► ofrece entonces Português y
  los otros dos idiomas.
- Idioma de la interfaz: el de KNULLI (`system.language` de `/userdata/system/batocera.conf`; si no, el de
  EmulationStation o `LANG`): español, English, français, português, italiano o Deutsch; si no se encuentra, español.
  `CACTUS_LANG=xx` o `--lang xx` lo fijan. Los errores habituales de nebula-pak (espacio, FAT32, imagen dañada,
  main.dol, cancelado) también salen traducidos; en una instalación nueva, el idioma del juego propuesto sigue al
  de la interfaz (español, francés o, para el resto, inglés).
- Controles: cruceta para elegir, A (derecha) convertir, B (abajo) salir o cancelar, X borrar el disco al terminar,
  MENU + START salir. `CACTUS_SWAP_AB=1` cambia A y B.
- `cactus-galaxy-wrapper --demo <carpeta>` escribe capturas PNG de cada pantalla y los fotogramas del cactus
  creciendo (sin pantalla ni SDL).

Se compila con `platform/knulli/pak/CMakeLists.txt` y `-DCACTUS_SDL2_PREFIX=<SDL2 para aarch64>`. Dibuja a
640x480 en memoria y lo muestra con SDL2 (la `libSDL2-2.0.so.0` de la consola).

Fuente: font8x8 de Daniel Hepper (<https://github.com/dhepper/font8x8>, revisión `8e279d2d`, dominio público;
`font8x8/README`), basada en las fuentes VGA de IBM, también de dominio público.
