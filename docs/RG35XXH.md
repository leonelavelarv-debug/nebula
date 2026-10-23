# Nebula en la Anbernic RG35XX H (KNULLI, aarch64): auditoría y primeras piezas

Rama `rg35xxh-arm64`. Objetivo: evaluar si la técnica de Nebula (recompilar el código PowerPC del juego a C++ y
correrlo nativo con un runtime propio) sirve para la RG35XX H, junto con la infraestructura NextOS del port de
Mario Kart Wii. **M** = medido aquí, **D** = leído del código, **H** = hipótesis con su medición pendiente.

## Punto de partida

- Nebula recompila **solo Super Mario Galaxy USA (RMGE01 rev 0)**. El recompilador comprueba el SHA-1 de
  `main.dol` y tiene 209 referencias a RMGE01 en `translate.rs` (direcciones de funciones, reemplazos nativos,
  mapa de funciones de Petari). No traduce Mario Kart Wii tal cual (D).
- El port NextOS de Mario Kart Wii (`mkwiita-nextos` V7) **no es una recompilación**: es una recreación nativa
  (mkwiita / MKW-3DS / Kinoko) que dibuja con GLES 2 y usa recursos que el preparador convierte del disco con
  Python y Node (41 etapas). Ya es aarch64, SDL3 estático y PortMaster (D).
- Por eso el puente entre ambos son **componentes y técnicas**, no "meter MKW en Nebula" directamente.

## Partes independientes de Windows y Direct3D 12 (D, M)

| Parte | Estado en aarch64 |
|---|---|
| Recompilador Rust (`crates/`, 50 461 líneas el núcleo) | Compila y corre en Linux (M). `windows-sys` solo en Windows |
| Frente GX: FifoParser, GxState, claves de shader, vertex loader, texturas | Compila para aarch64; `fifo_parser_tests` pasa (M) |
| FP PowerPC (`ppc_float`, Berkeley SoftFloat) | `ppc_float_tests` pasa en aarch64 (M) |
| ABI del módulo (`native_api.h`) | `native_api_tests`, `flat_dispatch_tests` pasan (M) |
| DSP (`dsp_alu`) | `dsp_alu_tests` pasa (M) |
| Cambio de contexto | Ya tiene ensamblador aarch64 (D) |
| Host SDL (`platform/sdl`) + renderer Vulkan | Existe para Android/Linux; Vulkan en el Mali-G31 de KNULLI está por verse (el probe lo comprueba) |
| Renderer D3D12, ventana/audio/entrada Win32, ray tracing, AO, upscaler | Solo Windows; sustituir |

De los 58 archivos portables del runtime que importan en el aparato, 57 compilan para aarch64 con un shim de
50 líneas (`platform/knulli/compat/nebula_posix_compat.h`: `getenv_s`, `_dupenv_s`, `fopen_s`, `_stricmp`…), y el
restante con la ruta de include correcta (M). Clasificación completa por archivo: `runtime_files.csv` del informe.

## ¿Genera el recompilador código compatible con ARM64?

- **Modo exacto** (`lower_ppc_code_range`): el C++ generado compila y da los mismos bits en x86-64 y aarch64 en
  todas las sondas (cargas big-endian, `fmadds` exacto, `divw` con INT_MIN/-1 y /0, paired singles, `fctiwz`) (M).
- **Modo de módulo optimizado** (native/fast float, vistas planas de RAM, registros en locales): dependía de
  `native_fast_paths.h`, que tenía `#error "fast float paired singles require x86-64"` y un `_mm_cvttsd_si32` sin
  guarda. Esta rama añade la rama AArch64 (vectores genéricos del compilador; bit a bit igual que la build SSE2 que
  se distribuye) y un `fctiwz` portable. Con eso pasan en aarch64 `native_fast_float_tests` y
  `native_hw_float_conversion_tests`, y las sondas dan bits idénticos en los dos ISA (M).
- **Fastmem** (`GALAXY_FASTMEM`) sigue siendo solo x86: su manejador de fallos decodifica instrucciones x86 bajo un
  VEH de Win32 (D). En aarch64 se usa el modo de vistas planas con comprobación de rango; un manejador SIGSEGV
  que decodifique LDR/STR+REV de AArch64 sería el siguiente paso si el perfil lo pide (H).
- Cobertura de opcodes Broadway: 191 de 207 se traducen. Faltan `eciwx ecowx eqv lhbrx lwbrx lswi lswx stswi
  stswx lwarx stwcx. nand stfiwx tw` (M, `opcode-coverage`). `ppc-histogram` dice cuáles usa un `main.dol`/`.rel`.
- Tamaño: 26–134 bytes de código aarch64 por instrucción PowerPC en las sondas (M). SMG tiene 1,35 M de
  instrucciones (D), así que su módulo sería del orden de 50–90 MiB de texto (H).

## Prueba mínima GLES + SDL: Nebula Probe

`platform/knulli/probe`, port de PortMaster (`nebulaprobe.zip`). Un solo binario aarch64 que solo depende de glibc
(carga SDL2 y GLES con `dlopen`). Mide en el aparato:

- RAM con el espacio de direcciones Wii mapeado como Nebula (reserva de 4 GiB, MEM1 24 MiB + MEM2 64 MiB, anillo GX
  de 16 MiB con doble mapeo `memfd`) y con un módulo simulado de 64 MiB.
- Código PowerPC traducido por el recompilador de Nebula, exacto y optimizado (binario aparte, como Nebula separa
  módulo y runtime), contra referencias nativas.
- El FifoParser y las claves de shader reales de Nebula por draw.
- GLES: versión, límites, las extensiones que el backend GX necesita (dual-source, framebuffer fetch, S3TC,
  depth24, geometry shaders, program binary, parallel compile…), compilación de un shader TEV "uber" y de 24
  especializados (tirones), draws por frame en un EFB de 640x528, relleno, subida de texturas, copia/lectura del
  EFB, peek de 1 píxel y ritmo de vsync. También busca Vulkan.
- Si el port NextOS de MKW está instalado al lado, su `nxcaps memsample` vigila la memoria del probe.

Validado aquí: aarch64 bajo qemu (CPU y memoria) y x86-64 con Mesa llvmpipe (camino GLES completo) (M). El doble
mapeo del anillo falla bajo qemu-user y pasa en nativo: lo decide el aparato.

## NXAccel: Nebula + infraestructura NextOS hoy

El preparador del port NextOS tarda más de 12 h en el aparato. Parte de la razón (D): decodifica texturas GX píxel a
píxel, Yaz0/Yay0 y DSP-ADPCM en Python puro, porque el SDK del port no trae `pyfastyaz0yay0`/`pyfastbti`.
`platform/knulli/nxaccel` aplica la técnica de Nebula (código nativo en las rutas calientes) **sin tocar el port**:
agrega tres archivos al `site-packages` del Python del preparador. Las salidas son **idénticas byte a byte** a las
de gclib/`nw4r_audio` (todos los formatos GX, paletas, Yaz0 compress/decompress, Yay0, ADPCM; M).

Con el propio Python aarch64 del port (bajo qemu, misma interpretación en ambos lados) (M):

| Función | Python | NXAccel | Veces |
|---|---|---|---|
| `decode_image` CMPR 256×256 | 277 ms | 1,8 ms | ×151 |
| `decode_image` RGB5A3 256×256 | 529 ms | 2,1 ms | ×247 |
| `decode_image` I4 256×256 | 296 ms | 1,3 ms | ×234 |
| Yaz0 decompress 256 KiB | 298 ms | 3,4 ms | ×87 |
| `decode_adpcm` 56 000 muestras | 178 ms | 7,7 ms | ×23 |

Cuánto baja el total de 12 h depende de qué fracción ocupan estas funciones (H): `NXACCEL_STATS` lo registra en una
preparación real. El instalador se autoprueba en el aparato y se retira solo si algo no coincide.

## Memoria y rendimiento antes de arrancar el juego

Presupuesto estimado de un juego de Wii recompilado en 1 GiB (H; el probe mide cada término):

| Término | MiB |
|---|---|
| MEM1 + MEM2 de la Wii | 88 |
| Anillo GX doble mapeado | 16 |
| Texto del módulo (residente parcial, es de archivo) | 30–90 |
| Driver GLES, EFB, caché de texturas | 60–120 |
| Audio, DSP, host | 20–30 |
| **Total** | **~215–345** |

El port NextOS de MKW se presupuesta en 300 MiB (`nxcaps.policy`), del mismo orden.

CPU (H): el módulo optimizado ejecuta 0,7–2,9 G instrucciones PowerPC/s en un Xeon de 2,1 GHz según la sonda (M).
Un Cortex-A53 a 1,5 GHz es del orden de 5–8× más lento por hilo, es decir 0,1–0,5 G/s, frente a ~0,5–0,7 G/s del
Broadway. La lógica de carrera de MKW a 60 fps probablemente no quepa en un núcleo sin optimización específica; el
render y el audio sí pueden ir en los otros tres núcleos. El probe en el aparato sustituye esta estimación por
números.

## Mediciones en el aparato (probe 0.1, KNULLI / Batocera 42, kernel 4.9.170) (M)

Informe completo: `platform/knulli/probe/results/rg35xxh-knulli42-probe-0.1.txt`.

| Medición | Resultado | Lectura |
|---|---|---|
| RAM | 973 MiB; 759 disponibles; 643 con la memoria Wii mapeada; 580 con un módulo de 64 MiB | Cabe con margen |
| CPU | 4× Cortex-A53 (0xd03) a 1512 MHz, gobernador performance; memcpy 1,06 GB/s | La memoria es el cuello de la CPU |
| Módulo optimizado, ALU | 7,28 ns por iteración de 8 instrucciones → 1,1 G instr/s (×1,21 del C nativo) | ~2× el Broadway |
| Módulo optimizado, carga+suma | 9,30 ns por 3 instrucciones → 0,32 G/s | ~0,4× el Broadway |
| Módulo optimizado, `fmadds` encadenado | 34,6 ns por 4 instrucciones → 0,12 G/s (×8,9 del C nativo) | ~0,15× el Broadway: el punto débil |
| Frente GX de Nebula | 1,18 µs por draw (1000 draws = 1,2 ms) | Aceptable en un núcleo aparte |
| GPU | Mali-G31, OpenGL ES 3.2 (driver r20p0), sin Vulkan | Backend GLES |
| Extensiones | Sí: ARM framebuffer fetch, depth24, geometry shader, program binary, buffer storage. No: dual-source, S3TC, parallel compile, clip control | Blend dual-source y logic-op por framebuffer fetch; CMPR decodificado en CPU; rango de Z en el vertex shader |
| Shaders | uber TEV 401 ms; especializados mediana 26,8 ms | El uber de Nebula no sirve aquí; especializados precompilados |
| Dibujo (uber) | 100 draws 35 ms, 500 draws 62 ms; relleno 17 Mpix/s | El uber dinámico ahoga al G31 |
| Dibujo (especializado) | 500 draws 27 ms a 640x528 | Falta separar relleno y vértices (probe 0.2) |
| EFB | copia a textura 1,58 ms; lectura completa 2,54 ms; peek 0,008 ms | Bien para pocas copias por frame |
| Anillo doble mapeado | "FALLO" | Falso negativo del probe 0.1 (reordenamiento del compilador); corregido en 0.2 |

Conclusión provisional: memoria y frente GX no bloquean. La CPU va bien en código entero y mal en punto flotante
encadenado, que es donde el camino `fast float` aún tiene ~9× de sobrecoste sobre el C nativo (primer objetivo de
optimización en ARM). La GPU es el límite: sin uber shaders, con programas precompilados y probablemente con el EFB a
media resolución (320x264; la pantalla es de 640x480). El probe 0.2 mide exactamente esas tres cosas.

## Mediciones en el aparato (probe 0.2) (M)

Informe: `platform/knulli/probe/results/rg35xxh-knulli42-probe-0.2.txt`. Lo nuevo respecto a 0.1:

| Medición | Resultado | Lectura |
|---|---|---|
| Anillo GX doble mapeado | OK | El fallo de 0.1 era del probe |
| Binarios de programa | 12/12 cargan, 0,30 ms cada uno (5,8 KiB) frente a 27,4 ms compilando | Los tirones de shader se eliminan con una caché precompilada |
| Frame vacío (clear + resolve) | 0,12 ms a 640x528 y a 320x264 | El costo fijo no es el framebuffer |
| Shader trivial, 1000 draws de 100 triángulos a 320x264 | 18,98 ms (~19 µs por draw) | El costo dominante es por draw/vértices, no por píxel |
| TEV especializado a 640x528 | 100 draws 7,65 ms; 500 → 28,6; 1000 → 49,4 | ~180 draws caben en 12 ms |
| TEV especializado a 320x264 | 100 draws 4,31 ms; 500 → 16,3; 1000 → 28,7 | ~350 draws en 12 ms; ~900 en 28 ms (30 fps) |
| Relleno TEV especializado | ~9–11 capas por frame a 60 fps | El píxel no es el límite |
| Vsync, solo presentar | 16,54 ms (60,5 Hz), 0 frames tarde | El panel y el swap van a 60 Hz |
| Vsync, 300 draws TEV a 320x264 | 16,80 ms (59,5 Hz), 1 tarde de 89 | Un frame ligero tipo Wii se sostiene a 60 fps |

Conclusión: con shaders especializados precompilados y el EFB a 320x264, la GPU sostiene ~300 draws a 60 fps o
~900 a 30 fps. El límite es el costo por draw (~19–30 µs). El probe 0.3 separa ese costo entre el driver (CPU) y los
vértices (GPU) y mide cuánto ahorra agrupar objetos en un draw instanciado, que es la técnica que lo bajaría.

## Mediciones en el aparato (probe 0.3) y veredicto (M, H)

Informe: `platform/knulli/probe/results/rg35xxh-knulli42-probe-0.3.txt`.

| Medición | Resultado | Lectura |
|---|---|---|
| 2000 draws de 2 triángulos | envío 14,6 ms (7,3 µs de driver por draw), total 26,1 ms (~13 µs de GPU por draw) | Cada draw cuesta en los dos lados aunque no dibuje casi nada |
| 20 draws de 10 000 triángulos | 15,8 ms → 12,7 Mtri/s | Rendimiento de vértices del G31 |
| 1000 draws de 100 triángulos | envío 7,3 ms, total 27,4 ms | ≈ 13 µs fijos + ~8 µs de vértices por draw |
| 1000 objetos instanciados en 1 draw | 21,6 ms frente a 26,6 ms | Agrupar ahorra ~19 %: quita el driver, no el costo de GPU |

Modelo de un frame en el G31 (H): `13 µs × draws + triángulos / 12,7 M + píxeles`; el driver (7,3 µs por draw) va en
el hilo de render, en otro núcleo. Con 600 draws y 50 000 triángulos a 320x264: ~8 + 4 + ~3 ≈ 15 ms (60 fps justos);
con 1200 draws, ~23 ms (30 fps). Los números de draws y triángulos de una carrera de MKW están por medir (H): Dolphin
los muestra en sus estadísticas de render con el disco del usuario.

CPU: el `fmadds` encadenado del módulo optimizado es la parte lenta (34,8 ns por iteración en el A53, in-order). Bajo
qemu, `GALAXY_SINGLE_FLOAT_ARITHMETIC=1` da los mismos bits en las sondas y ~18 % menos tiempo en ese bucle; el resto
del sobrecoste son las comprobaciones de rango de cada acceso, que solo un fastmem para AArch64 (manejador SIGSEGV que
decodifique LDR/STR) quitaría.

**Veredicto.** Memoria, frente GX, presentación a 60 Hz y tirones de shader (binarios precompilados) ya no bloquean.
Una versión recompilada de Mario Kart Wii en la RG35XX H es técnicamente viable apuntando a **30 fps, EFB a 320x264
y shaders especializados precompilados**; 60 fps solo en escenas ligeras. El riesgo principal pasa a ser la lógica
del juego en punto flotante en un solo núcleo del A53. Para jugar MKW hoy, el port NextOS (recreación nativa) sigue
siendo el camino práctico.

## Plan propuesto

0. **Medir en el aparato**: `Nebula Probe` y la autoprueba de NXAccel. Criterios para seguir con la vía
   recompilada: MemAvailable ≥ 450 MiB con la memoria Wii mapeada; GLES ≥ 3.0; mediana de compilación de shader
   especializado < 30 ms o `GL_OES_get_program_binary`; ≥ 800 draws en 12 ms; ALU del módulo optimizado ≤ 2× el
   nativo.
1. **Recompilador por perfil de juego**: sacar las constantes RMGE01 a un perfil; RMCE01 con mapa de funciones de
   decomp-toolkit para `main.dol` y `StaticR.rel`; cargador REL a dirección fija; los opcodes que diga
   `ppc-histogram`.
2. **Host Linux**: `platform/sdl` (SDL3) con el perfil SDL3 de NextOS (KMSDRM/Mali), lanzador V7 (`nxrun`,
   `nxcaps`, adaptador PortMaster); reemplazar las partes Win32 de `native_runtime.cpp`/`native_host.cpp`.
3. **Backend GX → GLES 3**: con la interfaz de `RendererVk`; `shader_gen` emite HLSL → glslang → SPIR-V →
   SPIRV-Cross → GLSL ES, compilado en el PC con caché de binarios; sin dual-source, logic-op, geometry shaders ni
   CMPR por hardware si el probe lo confirma.
4. **Compilación en el PC** (el aparato no puede: hasta 3,4 GB por compilador) y empaquetado NextOS/PortMaster.

## Construir

```sh
AARCH64_CXX=... AARCH64_CC=... VULKAN_HEADERS=.../Vulkan-Headers/include VOLK=.../volk platform/knulli/build.sh
cd platform/knulli/tools/ppc-lower && cargo build --release   # ppc-lower, opcode-coverage, ppc-histogram
```

## Super Mario Galaxy en la RG35XX H

Vía elegida: el **port nativo de Nebula** (Petari compilado, `native/`), no el módulo recompilado, porque evita el
sobrecoste de punto flotante medido arriba.

| Paso | Estado |
|---|---|
| Módulo del juego (`libsmg_game.so`, 1 891 unidades) para aarch64 / glibc 2.27 | **Compila y enlaza** (M) |
| Host sin gráficos (`nebula_native_host`) para aarch64 | Compila con tres arreglos: includes de logros/Discord fuera del bloque del renderer (rotos en 0592580), `-ldl -lpthread` para glibc < 2.34, `runtime_settings.cpp` + `input_bindings.cpp` en el host (M) |
| `nebula-pak`: `game.pak` desde el disco del usuario en Linux/Termux/macOS | `crates/nebula-import/src/bin/nebula-pak.rs` |
| Benchmark de la lógica en el aparato (`GALAXY_BENCH=1`, `GALAXY_TEST_GALAXY_TOUR=auto`) | Port `smgbench` (`platform/knulli/smgbench`). **Resultado (M):** las 44 galaxias a 60,0 fps (mínimo 59,9), hilo del juego entre 11 % y 51 % de un núcleo A53 (media 32 %), 12 min 18 s sin cuelgues; `smgbench/results/rg35xxh-knulli-bench-1.txt` |
| Renderer GX en OpenGL ES 3.2 (`runtime/src/gx/gles`) | **Escrito y probado en x86 con Mesa llvmpipe** (M): traducción de los 12 uber shaders, del shader de conversión EFB y de shaders especializados (HLSL → glslang → SPIR-V → SPIRV-Cross → GLSL ES 3.20); limpiezas, peeks de color y profundidad, campo de profundidad del puntero, copia EFB→XFB, presentación, orientación, profundidad invertida y cull de un draw GX real (`nebula_gles_renderer_tests`, 0 fallos). Sin probar aún con el juego (los datos solo están en el aparato del usuario) |
| Ventana/entrada/audio en KNULLI | `platform/knulli/game`: SDL2 del sistema (driver mali/fbdev), contexto GLES 3.2, contextos EGL compartidos para compilar shaders en hilos, mando como SDL GameController (esquema Nintendo por defecto), audio por cola SDL2 |
| Port `smg` (`platform/knulli/smg`) | Lanzador PortMaster con modo 4:3 nativo del juego y capturas de diagnóstico |

### Diseño del renderer GLES (H)

- Misma interfaz que `RendererVk` (GxBackend no cambia salvo `gx_backend.h`, que elige el backend, y la decodificación de
  texturas en hilos, apagada en GLES porque GL sube los bytes en cuanto TextureCache registra la subida).
- `gpu::Address` como en Vulkan: los bits altos nombran un buffer GL persistente (`EXT_buffer_storage`, coherente), los
  bajos el byte. Los anillos de vértices/índices/constantes/matrices son esos buffers (28/6/6/8 MiB por frame, tres
  frames en vuelo en GLES —la Mali termina un frame mucho después de grabarlo—; `GALAXY_GLES_RING_*_MB`).
- Campos de peek enteros con el EFB dividido: se convierte un texel por texel físico (320x264) y la CPU lo expande a
  640x528; el mapeo del PBO no tiene caché, así que se copia una sola vez en bloque (antes: 1.35 MB por campo, ~20 % del
  hilo de render en el aparato).
- El EFB es RGBA8 + profundidad `D32F` (exacta para el z24 invertido de GX: los peeks devuelven el valor escrito). Se dibuja
  con la fila superior de D3D primero (el vertex shader niega y), así copias, texturas y peeks conservan la orientación de
  D3D y solo la presentación voltea (`glBlitFramebuffer`).
- Paleta de matrices: SSBO en el vertex shader si el driver los tiene; si no, `usamplerBuffer` en la unidad 8
  (`GALAXY_GLES_PALETTE=texture` lo fuerza).
- Programas: binarios del driver en `data/shadercache/RMGE01/gles-<driver>/`; las claves vistas se vuelven a construir al
  arrancar en hilos con contextos EGL compartidos. `GALAXY_GLES_PENDING=wait|skip|uber`.
- Sin equivalente en GLES: blending de doble fuente (la alfa del TEV se mezcla con la alfa de salida), logic op, LOD bias
  de los samplers, depth clamp, líneas/puntos anchos. Interpolación, trazado de rayos y posproceso: no disponibles, como
  en Vulkan.

Probar en un PC Linux (sin juego):

```sh
cmake -S platform/knulli/game -B build-gles -G Ninja -DNEBULA_DEPS=<deps> -DNEBULA_SDL2_PREFIX=<sdl2>
ninja -C build-gles nebula_gles_shader_tests nebula_gles_renderer_tests
EGL_PLATFORM=surfaceless build-gles/nebula_gles_shader_tests
SDL_VIDEODRIVER=offscreen build-gles/nebula_gles_renderer_tests
```

Compilar el módulo y el host:

```sh
AARCH64_XTOOLS=/ruta/x-tools/aarch64-rpi3-linux-gnu cmake -S native -B build-aarch64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=platform/knulli/aarch64-toolchain.cmake \
  "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -static-libstdc++ -static-libgcc" \
  "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld -static-libstdc++ -static-libgcc"
ninja -C build-aarch64
```

### Perfil de bajos recursos (lanzador `smg`)

`NEBULA_PERFIL=bajo` (por defecto en el lanzador; `normal` lo apaga):

- **Salto de frames** (`GALAXY_FRAMESKIP=auto|0|1|2|3`, `GALAXY_FRAMESKIP_MAX`): un frame saltado se analiza entero
  (estado GX, display lists, tokens, peeks) pero no dibuja, no copia texturas ni limpia el EFB, así que su copia a
  pantalla repite el último frame dibujado. `auto` compara el tiempo ocupado del hilo de render (sin sus esperas al
  juego) con un frame de 60 Hz y salta el siguiente cuando va atrasado, como mucho 2 seguidos; `[frameskip]` en
  log.txt cuenta los saltados cada 600 frames. El juego conserva su velocidad y baja la fluidez.
- Cachés más pequeñas: display lists 192 MiB, copias EFB 64 MiB, pool de texturas 128 MiB, prefetch de disco 2 MiB.
- CPU, GPU y memoria en `performance` mientras se juega (`NEBULA_PERF=1`), restaurados al salir.
- `memoria:` en log.txt cada 60 s (RSS de nebula y memoria libre).
