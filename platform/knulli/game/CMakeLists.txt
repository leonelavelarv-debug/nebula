# Super Mario Galaxy on KNULLI handhelds (Anbernic RG35XX H, Mali-G31): the native game (native/CMakeLists.txt builds
# libsmg_game.so) with the shared GX layer on OpenGL ES 3.2 (runtime/src/gx/gles), SDL2 for the window, input and
# sound (the system's libSDL2-2.0.so.0 at run time). docs/RG35XXH.md has the build commands.
cmake_minimum_required(VERSION 3.22)
project(NebulaKnulli LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(nebula_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(NEBULA_DEPS "${nebula_root}/../deps" CACHE PATH "Folder of the dependency checkouts")
set(NEBULA_GLSLANG_SOURCE_DIR "${NEBULA_DEPS}/glslang" CACHE PATH "glslang source (External/spirv-tools inside)")
set(NEBULA_SPIRV_CROSS_DIR "${NEBULA_DEPS}/SPIRV-Cross" CACHE PATH "SPIRV-Cross source")
set(NEBULA_SDL2_PREFIX "" CACHE PATH "An SDL2 install (include/SDL2, lib/libSDL2-2.0.so) to build against")

# glslang with its HLSL front end and the SPIR-V optimizer (the HLSL legalization needs it).
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_OPT ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory("${NEBULA_GLSLANG_SOURCE_DIR}" glslang EXCLUDE_FROM_ALL)
file(GLOB glslang_spirv_headers "${NEBULA_GLSLANG_SOURCE_DIR}/SPIRV/*.h" "${NEBULA_GLSLANG_SOURCE_DIR}/SPIRV/*.hpp")
file(COPY ${glslang_spirv_headers} DESTINATION "${CMAKE_BINARY_DIR}/glslang-include/glslang/SPIRV")

# SPIRV-Cross, its GLSL back end only.
add_library(nebula_spirv_cross STATIC
    "${NEBULA_SPIRV_CROSS_DIR}/spirv_cfg.cpp"
    "${NEBULA_SPIRV_CROSS_DIR}/spirv_cross.cpp"
    "${NEBULA_SPIRV_CROSS_DIR}/spirv_cross_parsed_ir.cpp"
    "${NEBULA_SPIRV_CROSS_DIR}/spirv_parser.cpp"
    "${NEBULA_SPIRV_CROSS_DIR}/spirv_glsl.cpp")
target_include_directories(nebula_spirv_cross PUBLIC "${NEBULA_SPIRV_CROSS_DIR}")
target_compile_definitions(nebula_spirv_cross PUBLIC SPIRV_CROSS_NAMESPACE_OVERRIDE=spirv_cross)

add_library(nebula_gles_deps INTERFACE)
target_include_directories(nebula_gles_deps INTERFACE
    "${nebula_root}/third_party/khronos"
    "${CMAKE_BINARY_DIR}/glslang-include" "${NEBULA_GLSLANG_SOURCE_DIR}"
    "${NEBULA_GLSLANG_SOURCE_DIR}/External/spirv-tools/include")
target_compile_definitions(nebula_gles_deps INTERFACE NEBULA_GPU_GLES)
target_link_libraries(nebula_gles_deps INTERFACE glslang SPIRV glslang-default-resource-limits SPIRV-Tools-opt
    SPIRV-Tools-static nebula_spirv_cross)

# Uber shader sources as NUL-terminated char arrays, as in the root build.
set(galaxy_uber_embed_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/uber_shader_embed.cpp")
set(galaxy_uber_embed_body "")
foreach(stage vs ps)
    set(hlsl "${nebula_root}/runtime/src/gx/uber_${stage}.hlsl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hlsl}")
    file(READ "${hlsl}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    string(APPEND galaxy_uber_embed_body "extern const char galaxy_uber_${stage}_hlsl[] = {${hex}0x00};\n")
endforeach()
file(CONFIGURE OUTPUT "${galaxy_uber_embed_cpp}" CONTENT "namespace galaxy::gx {\n${galaxy_uber_embed_body}}\n" @ONLY)

set(rt "${nebula_root}/runtime/src")
add_library(galaxy_gx_gles STATIC
    ${rt}/gx/gx_shim.cpp
    ${rt}/gx/gx_state.cpp
    ${rt}/gx/frame_completion.cpp
    ${rt}/gx/render_config.cpp
    ${rt}/gx/renderer_common.cpp
    ${rt}/gx/upload_ring.cpp
    ${rt}/gx/texture_cache.cpp
    ${rt}/gx/texture_pack.cpp
    ${rt}/gx/texture_pack_codecs.cpp
    ${rt}/gx/movie_replacement.cpp
    ${rt}/gx/vertex_loader.cpp
    ${rt}/gx/shader_keys.cpp
    ${rt}/gx/efb_copy.cpp
    ${rt}/gx/line_point_raster.cpp
    ${rt}/gx/shader_gen.cpp
    ${rt}/gx/uber_shaders.cpp
    ${galaxy_uber_embed_cpp}
    ${rt}/gx/post_process_settings.cpp
    ${rt}/gx/transform_history.cpp
    ${rt}/gx/fifo_parser.cpp
    ${rt}/gx/gx_backend.cpp
    ${rt}/gx/game_lights.cpp
    ${rt}/gx/game_objects.cpp
    ${rt}/gx/rt_tev_eval.cpp
    ${rt}/gx/galaxy_select.cpp
    ${rt}/gx/frame_interpolation.cpp
    ${rt}/gx/interp_gpu.cpp
    ${rt}/gx/rt_surfaces.cpp
    ${rt}/gx/rt_materials.cpp
    ${rt}/gx/enhanced_lighting.cpp
    ${rt}/frame_telemetry.cpp
    ${rt}/perf_hint.cpp
    ${rt}/gx/game_asset_extract.cpp
    ${rt}/gx/vk/shader_compile_vk.cpp
    ${rt}/gx/gles/gles_context.cpp
    ${rt}/gx/gles/shader_translate_gles.cpp
    ${rt}/gx/gles/texture_backend_gles.cpp
    ${rt}/gx/gles/pipeline_cache_gles.cpp
    ${rt}/gx/gles/renderer_gles.cpp
    ${rt}/frame_cadence_diagnostics.cpp
    ${rt}/runtime_settings.cpp
    ${rt}/input_bindings.cpp
    ${rt}/host_pointer_events.cpp
    ${rt}/native_input.cpp
    ${rt}/pointer_control.cpp
    ${rt}/rumble.cpp
    ${rt}/switch_controller.cpp
    ${rt}/tilt.cpp
    ${rt}/jaudio_host_mixer.cpp
    ${rt}/atomic_file.cpp
    ${rt}/wii_device_data.cpp
    ${rt}/mii_import.cpp
    ${rt}/mii_formats.cpp
    ${rt}/mod_xml.cpp
    ${rt}/virtual_disc.cpp
    ${rt}/mods.cpp
    ${rt}/crash_report.cpp
    ${rt}/discord_presence.cpp
    ${rt}/achievements.cpp)
target_include_directories(galaxy_gx_gles PUBLIC "${nebula_root}/runtime/include")
target_include_directories(galaxy_gx_gles PRIVATE "${nebula_root}/third_party/smaa")
target_compile_options(galaxy_gx_gles PUBLIC "SHELL:-include ${nebula_root}/runtime/include/galaxy/posix_compat.h")
target_link_libraries(galaxy_gx_gles PUBLIC nebula_gles_deps Threads::Threads ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(galaxy_gx_gles PUBLIC -msse4.2)  # the x86 test build (simd128.h)
endif()

# The shader translation against the live driver: every uber variant and the renderer's own shaders, translated and
# linked (EGL, no window; GALAXY_GLES_TEST_DUMP=<dir> writes the GLSL).
add_executable(nebula_gles_shader_tests "${CMAKE_CURRENT_SOURCE_DIR}/gles_shader_tests.cpp")
target_link_libraries(nebula_gles_shader_tests PRIVATE galaxy_gx_gles)

if(NEBULA_SDL2_PREFIX)
    add_library(nebula_sdl2 INTERFACE)
    target_include_directories(nebula_sdl2 INTERFACE "${NEBULA_SDL2_PREFIX}/include/SDL2")
    target_link_libraries(nebula_sdl2 INTERFACE "${NEBULA_SDL2_PREFIX}/lib/libSDL2-2.0.so")
    add_executable(nebula_knulli
        "${nebula_root}/native/host/host_main.cpp"
        "${nebula_root}/runtime/src/native_thp_video.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/host_window_knulli.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/native_game_host_knulli.cpp"
        "${nebula_root}/platform/sdl/display_clock.cpp"
        "${nebula_root}/platform/sdl/thread_sampler.cpp")
    set_target_properties(nebula_knulli PROPERTIES OUTPUT_NAME nebula)
    target_include_directories(nebula_knulli PRIVATE "${nebula_root}/native/include" "${nebula_root}/runtime/include"
        "${nebula_root}/platform/sdl")
    target_compile_definitions(nebula_knulli PRIVATE NEBULA_HOST_RENDERER=1 NEBULA_HOST_SDL=1 NEBULA_HOST_SDL2=1)
    target_link_libraries(nebula_knulli PRIVATE galaxy_gx_gles nebula_sdl2)
    # The game module and SDL beside the executable.
    set_target_properties(nebula_knulli PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")

    # RendererGles through the window layer: clears, peeks, copies and the present, checked for orientation.
    add_executable(nebula_gles_renderer_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/gles_renderer_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/host_window_knulli.cpp"
        "${nebula_root}/platform/sdl/display_clock.cpp")
    target_include_directories(nebula_gles_renderer_tests PRIVATE "${nebula_root}/native/include"
        "${nebula_root}/platform/sdl")
    target_link_libraries(nebula_gles_renderer_tests PRIVATE galaxy_gx_gles nebula_sdl2)
endif()
