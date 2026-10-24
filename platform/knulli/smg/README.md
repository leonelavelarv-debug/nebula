# Super Mario Galaxy (Anbernic RG H700 con KNULLI/muOS, R36S RK3326 con ArkOS/dArkOS)

Super Mario Galaxy con gráficos en la consola: el código descompilado de Petari compilado nativo para ARM64, el host
de Nebula y su renderer GX en OpenGL ES 3.2 (Mali-G31). Primera versión de prueba.

1. Instálalo (hace falta PortMaster en la consola). Cada firmware guarda los ports en un sitio: el lanzador
   `Super Mario Galaxy.sh` donde el menú lo lista y la carpeta `smg/` en la carpeta de datos de PortMaster.
   - **Instalador** (`Instalar Super Mario Galaxy.sh`): cópialo a la carpeta de ports del menú y ábrelo desde Ports.
     Detecta el firmware, pone cada cosa en su sitio, da la ficha al menú y se borra. Si reinstalas, se conserva lo que
     ya tengas en `smg/` (game.pak, idioma, partidas).
   - **PortMaster** (`smg.zip`, su formato): déjalo en `autoinstall/` (en la carpeta de ports o en la de PortMaster) y
     abre PortMaster, que lo instala en las mismas rutas.

   | Firmware | Dónde copiar el instalador | Lanzador | Carpeta del juego |
   |---|---|---|---|
   | KNULLI, Batocera | `/userdata/roms/ports/` (`roms/ports` en la SD) | `/userdata/roms/ports/` | `/userdata/roms/ports/smg/` |
   | muOS | `/mnt/mmc/ROMS/Ports/` (o `/mnt/sdcard/ROMS/Ports/` en la SD 2) | `.../ROMS/Ports/` | `/mnt/mmc/ports/smg/` (o `/mnt/sdcard/ports/smg/`) |
   | ArkOS, dArkOS | `/roms/ports/` (`ports` en EASYROMS; `/roms2/ports/` con dos SD) | `/roms/ports/` | `/roms/ports/smg/` |
   | ROCKNIX, AmberELEC | `/storage/roms/ports/` (`roms/ports` en la SD) | `/storage/roms/ports/` | `/storage/roms/ports/smg/` |

   En ArkOS y dArkOS, EmulationStation se reinicia solo al terminar para que aparezca el juego. A mano también vale:
   en KNULLI, ArkOS, dArkOS y ROCKNIX se descomprime `smg.zip` dentro de `ports`; en muOS va `smg/` a `ports/` y
   `Super Mario Galaxy.sh` a `ROMS/Ports/`.
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
   libretro-thumbnails a `smg/` (sin red, la portada es la última captura de `smg/shots/`). La ficha (descripción,
   género, desarrollador, fecha...) sale en el idioma del juego elegido en Cactus Galaxy Wrapper (español, inglés,
   francés o portugués de Brasil) y `smg/gamelist.py` la pone donde la lee cada firmware:

   | Firmware | Dónde va la ficha | Cuándo se ve |
   |---|---|---|
   | KNULLI, Batocera, ROCKNIX | API local de EmulationStation (127.0.0.1:1234), que la guarda en `ports/gamelist.xml` | Enseguida |
   | ArkOS, dArkOS, AmberELEC | `ports/gamelist.xml` (EmulationStation sin API) | Al salir del juego: EmulationStation se reinicia una vez |
   | muOS | `MUOS/info/catalogue/External - Ports/` (`text/`, `box/`, `preview/` con el nombre del .sh) | Al volver al menú |

   En ArkOS y dArkOS, EmulationStation guarda al cerrarse su copia de la entrada y pisaría la ficha nueva; por eso,
   solo la primera vez y cuando cambias de idioma, el .sh lo para al salir del juego, escribe la ficha y lo vuelve a
   arrancar. La primera vez queda una copia en `ports/gamelist.xml.antes-smg`. `smg/gameinfo.xml` es la misma ficha
   para PortMaster.

La primera vez cada efecto nuevo compila su shader y el juego se detiene un momento (tirones); se guardan en
`smg/data/shadercache` y la siguiente vez cargan rápido.

Controles (esquema Nintendo): stick izquierdo = Nunchuk, A = saltar, B = saltar/atrás, X/Y/R2 = giro, L1 = Wii B
(disparar trozos de estrella), L2 = Z (agacharse), stick derecho = puntero, R1 = centrar puntero, cruceta = cámara,
START = +, SELECT = −. `NEBULA_PAD_SCHEME=xbox` en el lanzador cambia A y B de lado.

El lanzador adapta los controles al firmware y a la consola (lo que dice PortMaster; queda en la primera línea de
`smg/log.txt`):

Los controles son para consolas con dos sticks analógicos y cruceta.

| Firmware / consola | Salir |
|---|---|
| KNULLI, muOS (RG35XX H, RG35XX Pro, RG40XX H...: Allwinner H700) | MENU + START |
| ArkOS, dArkOS (R36S y clones: Rockchip RK3326) | SELECT + START |
| ROCKNIX, AmberELEC y otros | MENU + START o SELECT + START |

Si en tu firmware A y B salen cambiados, pon `NEBULA_FACE_LAYOUT=labels` al principio del lanzador.
`NEBULA_QUIT_COMBO=menu|select|both` fuerza la combinación para salir. Cactus Galaxy Wrapper usa el mismo perfil.

Para diagnóstico manda `smg/log.txt`, `smg/crashes/` y alguna captura de `smg/shots/` (cada 30 s, formato PPM).

Traducción al portugués de Brasil: Brazilian Warriors y Nintendo BR (Ray, xujozer, Joelson Kamppello, Angel Forgotten y ERT_Ganon); en João13 Traduções:
https://joao13traducoes.com/2018/01/wii-super-mario-galaxy-brazilian-warriors-e-nintendo-br/

Contiene código de Nebula (GPL-3.0), Petari (CC0), glslang (BSD/Apache-2.0), SPIRV-Cross (Apache-2.0) y
SPIRV-Tools (Apache-2.0); ningún dato de Nintendo: el juego viene de tu disco.
