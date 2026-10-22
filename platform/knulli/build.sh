#!/bin/sh
# Builds the RG35XX H (KNULLI, aarch64) pieces: the Nebula probe, its optimised CPU probe and NXAccel.
#   AARCH64_CXX  C++ cross compiler for aarch64-linux-gnu (glibc <= 2.27 sysroot recommended), e.g. a wrapper
#                around `clang++ --target=aarch64-linux-gnu --sysroot=...`
#   AARCH64_CC   the matching C compiler
#   VULKAN_HEADERS  Khronos Vulkan-Headers/include (headers only; the probe dlopens libvulkan)
#   VOLK         zeux/volk checkout (header only, for gpu_types.h's Vulkan branch)
#   STRIP        llvm-strip or aarch64-linux-gnu-strip
# Output: platform/knulli/out/
set -e
K=$(cd "$(dirname "$0")" && pwd); N=$(cd "$K/../.." && pwd); O=$K/out; mkdir -p $O/obj $O/ports
: "${AARCH64_CXX:?set AARCH64_CXX}"; : "${AARCH64_CC:?set AARCH64_CC}"; : "${VULKAN_HEADERS:?}"; : "${VOLK:?}"; STRIP=${STRIP:-llvm-strip}
COMPAT="-include $K/compat/nebula_posix_compat.h"
SF=$N/third_party/berkeley-softfloat-3/source
# Berkeley SoftFloat + galaxy_ppc_float, with the CMake project's file lists and definitions.
names=$(python3 -I - "$N/CMakeLists.txt" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
def block(n):
    m = re.search(r'set\(\s*' + n + r'\s+(.*?)\)', t, re.S)
    return [x for x in re.sub(r'#[^\n]*', '', m.group(1)).split() if re.fullmatch(r'\w+', x)]
print(' '.join('source/%s.c' % x for x in block('SOFTFLOAT_COMMON_NAMES')))
print(' '.join('source/8086-SSE/%s.c' % x for x in block('SOFTFLOAT_SPECIALIZE_NAMES')))
PY
)
objs=""
for f in $names; do o=$O/obj/$(basename $f .c).o; $AARCH64_CC -O2 -mcpu=cortex-a53 -ffp-contract=off -c -DSOFTFLOAT_FAST_INT64=1 -DSOFTFLOAT_FAST_DIV32TO16=1 -DSOFTFLOAT_FAST_DIV64TO32=1 -DINLINE_LEVEL=5 -I$N/runtime/softfloat -I$SF/include -I$SF/8086-SSE $N/third_party/berkeley-softfloat-3/$f -o $o; objs="$objs $o"; done
$AARCH64_CXX -std=c++20 -O2 -mcpu=cortex-a53 -ffp-contract=off -fdeclspec $COMPAT -c -DSOFTFLOAT_FAST_INT64=1 -I$N/runtime/include -I$N/runtime/softfloat -I$SF/include $N/runtime/src/ppc_float.cpp -o $O/obj/ppc_float.o
rm -f $O/libgalaxy_ppc_float.a; ar rcs $O/libgalaxy_ppc_float.a $O/obj/ppc_float.o $objs
FLAGS="-std=c++20 -O2 -mcpu=cortex-a53 -ffp-contract=off -fno-strict-aliasing -fno-math-errno -fdeclspec -w $COMPAT"
P=$K/probe/src
$AARCH64_CXX $FLAGS -DNEBULA_GPU_VULKAN=1 -DVK_NO_PROTOTYPES -I$P -I$N/runtime/include -I$N/runtime/src -I$N/runtime/src/gx -I$VULKAN_HEADERS -I$VOLK \
  $P/probe_main.cpp $P/sys_probe.cpp $P/recomp_probes.cpp $P/gx_fifo_bench.cpp \
  $N/runtime/src/gx/fifo_parser.cpp $N/runtime/src/gx/gx_state.cpp $N/runtime/src/gx/shader_keys.cpp $N/runtime/src/gx/render_config.cpp $N/runtime/src/atomic_file.cpp \
  $O/libgalaxy_ppc_float.a -static-libstdc++ -static-libgcc -ldl -lpthread -Wl,--gc-sections -o $O/nebula-probe
$AARCH64_CXX $FLAGS -DGALAXY_FAST_FLOAT=1 -DGEN_DIR=gen_fast -I$P -I$N/runtime/include $P/cpu_fast_main.cpp $O/libgalaxy_ppc_float.a -static-libstdc++ -static-libgcc -o $O/nebula-probe-cpu-fast
$AARCH64_CXX -std=c++20 -O2 -mcpu=cortex-a53 -fPIC -shared -fvisibility=hidden -static-libstdc++ -static-libgcc $K/nxaccel/src/nxaccel.cpp -o $O/libnxaccel.so
$STRIP $O/nebula-probe $O/nebula-probe-cpu-fast $O/libnxaccel.so
# PortMaster packages.
rm -rf $O/stage && mkdir -p "$O/stage/probe/nebulaprobe/licenses" "$O/stage/nx/nxaccel-mkw/licenses" "$O/stage/nx/nxaccel-mkw/tests"
cp "$K/probe/Nebula Probe.sh" $O/stage/probe/ && cp $O/nebula-probe $O/nebula-probe-cpu-fast $K/probe/nebula-probe.gptk $K/probe/port.json $K/probe/README.md $O/stage/probe/nebulaprobe/
cp $N/LICENSE $O/stage/probe/nebulaprobe/licenses/GPL-3.0.txt
cp "$K/nxaccel/NXAccel Mario Kart Wii.sh" $O/stage/nx/ && cp -r $K/nxaccel/python $K/nxaccel/README.md $O/stage/nx/nxaccel-mkw/ && cp $K/nxaccel/tests/test_equivalence.py $O/stage/nx/nxaccel-mkw/tests/
cp $O/libnxaccel.so $O/stage/nx/nxaccel-mkw/python/nxaccel/ && cp $N/LICENSE $O/stage/nx/nxaccel-mkw/licenses/GPL-3.0.txt
find $O/stage -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
chmod +x "$O/stage/probe/Nebula Probe.sh" "$O/stage/nx/NXAccel Mario Kart Wii.sh" $O/stage/probe/nebulaprobe/nebula-probe $O/stage/probe/nebulaprobe/nebula-probe-cpu-fast
(cd $O/stage/probe && rm -f $O/ports/nebulaprobe.zip && zip -qr $O/ports/nebulaprobe.zip .)
(cd $O/stage/nx && rm -f $O/ports/nxaccel-mkw.zip && zip -qr $O/ports/nxaccel-mkw.zip .)
echo "out: $O/ports/nebulaprobe.zip $O/ports/nxaccel-mkw.zip"
