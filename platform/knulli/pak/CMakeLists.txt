# nebula-pak for the console: ISO/RVZ/WIA/WBFS/GCZ/CISO -> game.pak, with no Rust. Static apart from glibc.
#   cmake -S platform/knulli/pak -B build-pak -DCMAKE_TOOLCHAIN_FILE=<aarch64 toolchain> -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.20)
project(nebula_pak C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
get_filename_component(NEBULA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(TP "${NEBULA_ROOT}/third_party")

# zlib (GCZ): its sources directly; zconf.h is the stock one.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/zlib")
configure_file("${TP}/archive/zlib/zconf.h.in" "${CMAKE_BINARY_DIR}/zlib/zconf.h" COPYONLY)
add_library(pak_zlib STATIC
    ${TP}/archive/zlib/adler32.c ${TP}/archive/zlib/crc32.c ${TP}/archive/zlib/inffast.c
    ${TP}/archive/zlib/inflate.c ${TP}/archive/zlib/inftrees.c ${TP}/archive/zlib/zutil.c)
target_include_directories(pak_zlib PUBLIC "${CMAKE_BINARY_DIR}/zlib" "${TP}/archive/zlib")
target_compile_definitions(pak_zlib PRIVATE HAVE_UNISTD_H)

# liblzma (WIA LZMA/LZMA2): only the raw LZMA1/LZMA2 decoders, with a small config.h (lzma_config.h here).
set(XZL "${TP}/archive/xz/src/liblzma")
add_library(pak_lzma STATIC
    ${XZL}/common/common.c ${XZL}/common/filter_common.c ${XZL}/common/filter_decoder.c
    ${XZL}/lz/lz_decoder.c ${XZL}/lzma/lzma_decoder.c ${XZL}/lzma/lzma2_decoder.c
    ${XZL}/lzma/lzma_encoder_presets.c)
target_include_directories(pak_lzma PUBLIC "${XZL}/api"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lzma" "${XZL}/common" "${XZL}/lz" "${XZL}/lzma" "${XZL}/rangecoder"
            "${XZL}/check" "${XZL}/simple" "${XZL}/delta" "${TP}/archive/xz/src/common")
target_compile_definitions(pak_lzma PUBLIC LZMA_API_STATIC PRIVATE HAVE_CONFIG_H)
target_compile_options(pak_lzma PRIVATE -w)

# Zstandard (RVZ), decompression only.
file(GLOB PAK_ZSTD_SOURCES "${TP}/disc/zstd/lib/common/*.c" "${TP}/disc/zstd/lib/decompress/*.c")
add_library(pak_zstd STATIC ${PAK_ZSTD_SOURCES})
target_include_directories(pak_zstd PUBLIC "${TP}/disc/zstd/lib")
target_compile_definitions(pak_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0 ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=ZSTD_)

# bzip2 (WIA).
add_library(pak_bzip2 STATIC
    ${TP}/disc/bzip2/blocksort.c ${TP}/disc/bzip2/bzlib.c ${TP}/disc/bzip2/compress.c ${TP}/disc/bzip2/crctable.c
    ${TP}/disc/bzip2/decompress.c ${TP}/disc/bzip2/huffman.c ${TP}/disc/bzip2/randtable.c)
target_include_directories(pak_bzip2 PUBLIC "${TP}/disc/bzip2")
target_compile_options(pak_bzip2 PRIVATE -w)

add_executable(nebula-pak nebula_pak.cpp)
target_link_libraries(nebula-pak PRIVATE pak_zlib pak_lzma pak_zstd pak_bzip2 pthread)
target_compile_options(nebula-pak PRIVATE -O2 -Wall -Wextra)
target_link_options(nebula-pak PRIVATE -static-libstdc++ -static-libgcc -Wl,--gc-sections)

# Cactus Galaxy Wrapper (platform/knulli/cactus): the console's interface around nebula-pak, against an SDL2 install
# (include/SDL2, lib/libSDL2-2.0.so; the console's own libSDL2-2.0.so.0 at run time).
set(CACTUS_SDL2_PREFIX "" CACHE PATH "SDL2 install for cactus-galaxy-wrapper (empty: not built)")
if(CACTUS_SDL2_PREFIX)
    add_executable(cactus-galaxy-wrapper "${NEBULA_ROOT}/platform/knulli/cactus/cactus_wrapper.cpp")
    target_include_directories(cactus-galaxy-wrapper PRIVATE "${CACTUS_SDL2_PREFIX}/include/SDL2" "${TP}/stb")
    target_link_libraries(cactus-galaxy-wrapper PRIVATE "${CACTUS_SDL2_PREFIX}/lib/libSDL2-2.0.so" pthread m)
    target_compile_options(cactus-galaxy-wrapper PRIVATE -O2 -Wall -Wextra)
    target_link_options(cactus-galaxy-wrapper PRIVATE -static-libstdc++ -static-libgcc -Wl,--gc-sections)
endif()
