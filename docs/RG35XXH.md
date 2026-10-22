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
