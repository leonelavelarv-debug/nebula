# NXAccel para Mario Kart Wii (NextOS)

El preparador del port NextOS de Mario Kart Wii (`mkwiita-nextos`) convierte el disco en el propio
aparato con Python. Tres de sus bucles internos (decodificar texturas GX con `gclib.texture_utils.decode_image`,
descomprimir Yaz0/Yay0 y decodificar DSP-ADPCM con `nw4r_audio.decode_adpcm`) corren en Python puro porque el SDK
del port no trae los módulos nativos opcionales que gclib busca. NXAccel los reemplaza por código nativo aarch64
con **el mismo resultado byte a byte** (la autoprueba lo comprueba en el aparato antes de dejarlo instalado).

## Instalar
1. Copie `NXAccel Mario Kart Wii.sh` y la carpeta `nxaccel-mkw/` a `ports`, junto a `mkwiita-nextos/`.
2. Ejecútelo una vez desde el menú Ports. Tarda menos de un minuto; el resultado queda en `nxaccel-mkw/log.txt`.
3. Abra Mario Kart Wii y prepare los datos como siempre.

## Qué cambia
Solo agrega `nxaccel/`, `pyfastyaz0yay0.py` y `nxaccel.pth` al `site-packages` del Python del preparador.
No modifica ningún archivo del port. Si algo no coincide, la función nativa cede y corre el Python original.
`NXACCEL=0` lo desactiva sin desinstalar. `NXACCEL_STATS=/ruta/archivo.jsonl` registra llamadas y segundos
por función en cada proceso (para medir cuánto del tiempo total cubre).

## Desinstalar
Cree el archivo vacío `nxaccel-mkw/UNINSTALL` y ejecute de nuevo el `.sh`.

Basado en la técnica de Nebula (código nativo en las rutas calientes). GPL-3.0. Sin código ni datos de Nintendo.
