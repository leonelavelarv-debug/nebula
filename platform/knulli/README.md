# platform/knulli — consolas ARM64 con PortMaster (KNULLI, muOS, ArkOS, dArkOS, ROCKNIX)

El port de Super Mario Galaxy y Cactus Galaxy Wrapper: ver el [README del repositorio](../../README.md) y
[docs/RG35XXH.md](../../docs/RG35XXH.md).

| Carpeta | Qué es |
|---|---|
| `pak/` | `nebula-pak`: convertidor nativo de discos a `game.pak` (ISO/WBFS/CISO/GCZ/WIA/RVZ, parches xdelta) |
| `cactus/` | Cactus Galaxy Wrapper: interfaz SDL2 dibujada a mano a 640x480, con textos en 6 idiomas |
| `game/` | Host de la consola (ventana, mandos, perfiles por firmware) y pruebas del renderer GLES |
| `smg/` | El port: lanzador, `gamelist.py` (ficha por firmware), `port.json`, instalador y `empaquetar.sh` |
| `compat/` | Shim POSIX de los nombres de la CRT de MSVC que usa el runtime |
| `probe/` | Nebula Probe: port de PortMaster que mide memoria, CPU y GPU del aparato |
| `nxaccel/` | NXAccel: código nativo para el preparador del port NextOS de Mario Kart Wii |
| `recomp-probes/` | Sondas PowerPC (ensamblador mínimo) y arneses que corren el código traducido |
| `tools/ppc-lower/` | `ppc-lower`, `opcode-coverage`, `ppc-histogram` sobre `nebula-recomp-core` |
| `build.sh` | Construye la sonda y NXAccel para aarch64 y sus zips de PortMaster en `out/ports/` |
