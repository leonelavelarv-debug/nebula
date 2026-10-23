# Super Mario Galaxy (RG35XX H, KNULLI)

Super Mario Galaxy con gráficos en la consola: el código descompilado de Petari compilado nativo para ARM64, el host
de Nebula y su renderer GX en OpenGL ES 3.2 (Mali-G31). Primera versión de prueba.

1. Copia `Super Mario Galaxy.sh` y la carpeta `smg/` a `ports` (el zip ya viene así: se descomprime dentro de `ports`).
2. Ejecuta **Super Mario Galaxy** desde Ports. La primera vez se abre **Cactus Galaxy Wrapper** (va dentro del port):
   - Sin el juego, te pide el disco: copia tu ISO o RVZ (también WIA, WBFS, GCZ o CISO) de Super Mario Galaxy USA
     (RMGE01) a `smg/gamedata/` (o a `roms/wii/`) y aparece solo. Eliges el idioma del juego (Español, English,
     Français y, con el parche de la traducción en `smg/patches/`, Português do Brasil, que se aplica al convertir)
     y lo convierte ahí mismo en `smg/gamedata/game.pak` con `smg/nebula-pak` (sin Termux ni otro equipo). Tarda lo
     que la tarjeta en escribir ~4 GB; hacen falta ~3.4 GB libres (ext4) o ~4.3 GB (exFAT; FAT32 no sirve).
   - Con el juego ya en `smg/gamedata/`, te ofrece los idiomas del juego.
   Después el juego arranca directamente; **mantén SELECT (o L1) al abrirlo** para volver a las opciones. Sale en el
   idioma de tu KNULLI. `NEBULA_PAK_DELETE_IMAGE=1` en el lanzador borra la imagen al terminar.
   - También se puede hacer fuera: `bash tools/cactus-galaxy-wrapper.sh <imagen> [carpeta smg]` en Termux, Linux o
     macOS.
3. MENU (Function) + START sale, como en el resto de KNULLI.
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
