# Super Mario Galaxy (RG35XX H, KNULLI)

Super Mario Galaxy con gráficos en la consola: el código descompilado de Petari compilado nativo para ARM64, el host
de Nebula y su renderer GX en OpenGL ES 3.2 (Mali-G31). Primera versión de prueba.

1. Copia `Super Mario Galaxy.sh` y la carpeta `smg/` a `ports` (el zip ya viene así: se descomprime dentro de `ports`).
2. El juego: pon tu disco (ISO, RVZ, WIA, WBFS, GCZ o CISO) en `smg/gamedata/` (o en `roms/wii/`) y abre
   **Cactus Galaxy Wrapper** en Ports (o directamente el juego, que lo abre solo si falta `game.pak`). Lo convierte
   ahí mismo en `smg/gamedata/game.pak` con `smg/nebula-pak` (nativo, viene en el port: sin Termux ni otro equipo),
   con el avance en pantalla y un saguaro que crece hasta florecer. Tarda lo que la tarjeta en escribir ~4 GB (unos
   minutos); hacen falta ~3.4 GB libres (ext4) o ~4.3 GB (exFAT; FAT32 no sirve, no admite ficheros de más de 4 GB).
   Cruceta: elegir; A: convertir; B: salir o cancelar; X: borrar el disco al terminar. Solo vale el disco USA
   (RMGE01).
   - También se puede hacer fuera: `bash tools/cactus-galaxy-wrapper.sh <imagen> [carpeta smg]` en Termux, Linux o
     macOS.
3. Ejecuta **Super Mario Galaxy** desde Ports. MENU (Function) + START sale, como en el resto de KNULLI.
4. Portada y ficha: el primer arranque con red baja la portada, la captura y la pantalla de título de
   libretro-thumbnails a `smg/` y escribe la ficha (descripción, desarrollador, fecha...) en `ports/gamelist.xml`
   (`smg/gamelist.py`; la primera vez guarda una copia en `gamelist.xml.antes-smg`). La ficha se da a EmulationStation por su API local (127.0.0.1:1234, la de los scrapers de Batocera) para que no
   la pise al salir del juego; sin red, la portada es la última captura de `smg/shots/`. `smg/gameinfo.xml` es la
   misma ficha para PortMaster.

La primera vez cada efecto nuevo compila su shader y el juego se detiene un momento (tirones); se guardan en
`smg/data/shadercache` y la siguiente vez cargan rápido.

Controles (esquema Nintendo): stick izquierdo = Nunchuk, A = saltar, B = saltar/atrás, X/Y/R2 = giro, L1 = Wii B
(disparar trozos de estrella), L2 = Z (agacharse), stick derecho = puntero, R1 = centrar puntero, cruceta = cámara,
START = +, SELECT = −. `NEBULA_PAD_SCHEME=xbox` en el lanzador cambia A y B de lado.

Para diagnóstico manda `smg/log.txt`, `smg/crashes/` y alguna captura de `smg/shots/` (cada 30 s, formato PPM).

Contiene código de Nebula (GPL-3.0), Petari (CC0), glslang (BSD/Apache-2.0), SPIRV-Cross (Apache-2.0) y
SPIRV-Tools (Apache-2.0); ningún dato de Nintendo: el juego viene de tu disco.
