# Nebula Probe (RG35XX H / KNULLI)

Prueba mínima antes de intentar arrancar un juego de Wii recompilado al estilo Nebula.
Dura ~1 minuto. Al terminar muestra el resumen; cualquier botón sale (SELECT+START también).

Resultados en esta carpeta:
- `nebula-probe-report.txt` — informe legible
- `nebula-probe-report.json` — los mismos números, para compararlos
- `nxcaps-memsample.json` — sólo si el port NextOS de Mario Kart Wii (`mkwiita-nextos`) está instalado junto a este
- `log.txt` — salida completa

Opciones (editar la línea `./nebula-probe` del .sh): `--quick` (más corto), `--headless` (sin GPU),
`--module-mib N` (tamaño simulado del módulo del juego, 64 por defecto).

Qué mide: RAM con el espacio de direcciones Wii mapeado (MEM1 24 MiB + MEM2 64 MiB + anillo GX
16 MiB doble mapeado), código PowerPC traducido por el recompilador de Nebula ejecutándose en
los Cortex-A53, el FifoParser/GxState/claves de shader de Nebula, versión GLES y extensiones,
tiempo de compilación de shaders tipo TEV (tirones), draws por frame en un EFB de 640x528,
relleno, subida de texturas, copias/lecturas del EFB y ritmo de vsync. También busca Vulkan.

Contiene código de Nebula (GPL-3.0). No contiene código ni datos de Nintendo.
