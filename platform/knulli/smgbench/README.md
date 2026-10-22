# Mario Galaxy Bench (RG35XX H)

Primer paso del port de Super Mario Galaxy: el juego completo (código descompilado de Petari compilado nativo para
ARM64, con el host de Nebula) corriendo **sin gráficos** en la consola. Recorre cada galaxia del disco ~15 s y anota
cada 10 s los fps de la lógica, el uso de CPU del hilo del juego y la galaxia (`bench.txt`, líneas `[bench]`).

1. Haz `game.pak` de tu disco con `nebula-pak` (ver INSTALACION en el informe) y cópialo a `smgbench/gamedata/`.
2. Copia `Mario Galaxy Bench.sh` y `smgbench/` a `ports`. Ejecuta. La pantalla queda negra hasta 40 min.
3. Manda `smgbench/bench.txt` (y `crashes/` si aparece algo).

`fps=60` y `sim_cpu` < 100 % en una galaxia: la lógica cabe a 60 fps ahí. `fps≈30`: cabe a 30. Contiene código de
Nebula (GPL-3.0) y de Petari (CC0); ningún dato de Nintendo: el juego viene de tu disco.
