# platform/knulli — Anbernic RG35XX H (KNULLI, aarch64)

Ver [docs/RG35XXH.md](../../docs/RG35XXH.md).

| Carpeta | Qué es |
|---|---|
| `compat/` | Shim POSIX de los nombres de la CRT de MSVC que usa el runtime |
| `probe/` | Nebula Probe: port de PortMaster que mide memoria, CPU y GPU del aparato |
| `nxaccel/` | NXAccel: código nativo para el preparador del port NextOS de Mario Kart Wii |
| `recomp-probes/` | Sondas PowerPC (ensamblador mínimo) y arneses que corren el código traducido |
| `tools/ppc-lower/` | `ppc-lower`, `opcode-coverage`, `ppc-histogram` sobre `nebula-recomp-core` |
| `build.sh` | Construye todo para aarch64 y los zips de PortMaster en `out/ports/` |
