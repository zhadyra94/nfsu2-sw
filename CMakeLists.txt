cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Need for Speed: Underground 2 (Xbox, NTSC-U) - Static Recompilation
#
# Hosts: Windows (MSVC, D3D11), Linux (GCC, SDL2 + OpenGL) and Nintendo
# Switch homebrew (devkitA64 + libnx, built with switch/build.sh).
# ============================================================================

project(nfsu2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# The lifted code is ~100 MB of C generated from the retail XBE. It is never
# committed; point this at wherever tools.recomp --gen-dir wrote it.
set(NFSU2_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "Directory holding the tools.recomp output (recomp_*.c, recomp_funcs.h)")

if(NOT EXISTS "${NFSU2_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "No generated code in ${NFSU2_GEN_DIR}. Run tools.recomp "
                        "with --gen-dir, or pass -DNFSU2_GEN_DIR=<dir>.")
endif()

# The Vulkan renderer (xboxrecomp nv2a_vk) instead of OpenGL. On the Switch it
# links mesa-switch's static NVK (NVK_SDK) and builds nfsu2x-vulkan.nro.
option(NFSU2_VULKAN "Render with Vulkan instead of OpenGL" OFF)
set(XBOXRECOMP_VULKAN ${NFSU2_VULKAN} CACHE BOOL "" FORCE)

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${NFSU2_GEN_DIR}/*.c")

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/switch_nx.c
    src/text_patch.c
    src/movie_crop.c
src/nic.c
    src/nfsu2_state.c
)

if(IOS)
    list(APPEND GAME_SOURCES xboxrecomp/src/platform/ios_touch_overlay.m
                             xboxrecomp/src/platform/ios_keyboard.m
                             xboxrecomp/src/platform/ios_vblank.m
                             src/ios_launcher.m)
    set_source_files_properties(xboxrecomp/src/platform/ios_touch_overlay.m
                                xboxrecomp/src/platform/ios_keyboard.m
                                xboxrecomp/src/platform/ios_vblank.m
                                src/ios_launcher.m PROPERTIES
        COMPILE_FLAGS "-I${CMAKE_SOURCE_DIR}/xboxrecomp/src/input -fobjc-arc")
endif()

if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
else()
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Movies: VP6 decoded by FFmpeg instead of the lifted On2 decoder
# (src/movie_vp6.c). NFSU2_FFMPEG_DIR is an install of the minimal LGPL
# build from tools/build_ffmpeg_vp6.sh (libavcodec with only the vp6
# decoder); without it the lifted decoder plays the movies.
set(NFSU2_FFMPEG_DIR "" CACHE PATH "FFmpeg (vp6 decoder) install prefix")
if(NFSU2_FFMPEG_DIR)
    if(NOT EXISTS "${NFSU2_FFMPEG_DIR}/lib/libavcodec.a")
        message(FATAL_ERROR "no libavcodec.a in ${NFSU2_FFMPEG_DIR}/lib (tools/build_ffmpeg_vp6.sh)")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE src/movie_vp6.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_NATIVE_VP6=1)
    # BEFORE: the Switch portlibs include dir has devkitPro's (GPL) FFmpeg headers.
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${NFSU2_FFMPEG_DIR}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${NFSU2_FFMPEG_DIR}/lib/libavcodec.a" "${NFSU2_FFMPEG_DIR}/lib/libavutil.a")
endif()

if(NFSU2_VULKAN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_VULKAN=1)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NFSU2_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11 dxgi dxguid xinput winmm dbghelp)
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
        target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    endif()
else()
    # The generated chunks are huge straight-line functions; -O1 keeps GCC's
    # memory and time per chunk sane while still allocating registers. The
    # Switch's CPU is the bottleneck there, so it gets -O2.
    if(NINTENDO_SWITCH)
        set(NFSU2_GEN_OPT "-O2" CACHE STRING "Optimisation for the lifted code")
    else()
        set(NFSU2_GEN_OPT "-O1" CACHE STRING "Optimisation for the lifted code")
    endif()
    # NFSU2_LTO: link-time optimisation of the lifted code and its manual
    # overrides, so lifted callers inline each other and the native leaves
    # (_ftol2, ...). The runtime and renderers are compiled as usual.
    # NFSU2_LTO_JOBS parallel LTRANS jobs (memory: ~1-2 GB each).
    option(NFSU2_LTO "Link-time optimisation of the lifted code" OFF)
    set(NFSU2_LTO_JOBS "4" CACHE STRING "Parallel LTO link jobs")
    set(NFSU2_GEN_FLAGS "${NFSU2_GEN_OPT};-w;-fno-strict-aliasing;-fwrapv")
    if(NFSU2_LTO)
        list(APPEND NFSU2_GEN_FLAGS "-flto=auto")
        set_property(SOURCE src/recomp_manual.c APPEND PROPERTY
            COMPILE_OPTIONS "-flto=auto;-fno-strict-aliasing;-fwrapv")
        target_link_options(${PROJECT_NAME} PRIVATE
            "-flto=${NFSU2_LTO_JOBS}" "-flto-partition=balanced" ${NFSU2_GEN_OPT})
    endif()
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${NFSU2_GEN_FLAGS}")
    # Clang (the iOS build) fuses a*b+c into one FMA by default and ignores
    # GCC's optimize("fp-contract=off"), so the native math overrides drifted
    # from the lifted code in the last bit (211 fused ops on arm64). The
    # lifted code is one op per statement and is not affected.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set_property(SOURCE src/recomp_manual.c APPEND PROPERTY
            COMPILE_OPTIONS "-ffp-contract=off")
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()

# ── Nintendo Switch homebrew (devkitA64 + libnx) ────────────────
#
# Build with switch/build.sh. The NRO reads the extracted disc from
# sdmc:/switch/nfsu2x/game/ (never an ISO); logs and nfsu2x_env.txt live in
# sdmc:/switch/nfsu2x/.
if(NINTENDO_SWITCH)
    set(NFSU2_SWITCH_APP_NAME "NFS Underground 2 (Xbox recomp)" CACHE STRING
        "Title shown in the homebrew menu")
    set(NFSU2_SWITCH_AUTHOR "nfsu2-xbox" CACHE STRING "Author shown in the homebrew menu")
    # The release lives here, not in the cache: a cached default stays at
    # whatever the build directory was first configured with (the 0.4.5 NRO
    # said 0.4.3). -DNFSU2_SWITCH_VERSION_OVERRIDE=x sets another one.
    set(NFSU2_SWITCH_VERSION "0.5")
    if(NFSU2_SWITCH_VERSION_OVERRIDE)
        set(NFSU2_SWITCH_VERSION "${NFSU2_SWITCH_VERSION_OVERRIDE}")
    endif()
    set(NFSU2_SWITCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg" CACHE FILEPATH
        "256x256 JPEG icon (libnx default when empty)")

    if(NFSU2_VULKAN)
        # No GL: SDL is only the audio output; NVK comes with nv2a_vk.
        target_link_libraries(${PROJECT_NAME} PRIVATE nx m)
    else()
        # switch-sdl2's GL path is EGL on switch-mesa.
        target_link_libraries(${PROJECT_NAME} PRIVATE EGL glapi drm_nouveau nx m)
    endif()
    # SDL's audio thread gets a real-time priority and audren gaps are
    # counted (switch_nx.c, "SDL's audio thread").
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:--wrap=SDL_SYS_SetThreadPriority"
        "LINKER:--wrap=audrvVoiceAddWaveBuf")
    set_property(SOURCE src/switch_nx.c APPEND PROPERTY
        INCLUDE_DIRECTORIES "${XBOXRECOMP_DIR}/src/apu")
    # switch_nx.c's loading screen asks EGL for the on-screen size.
    target_include_directories(${PROJECT_NAME} PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")

    set(NFSU2_NACP "${CMAKE_CURRENT_BINARY_DIR}/nfsu2x.nacp")
    nx_generate_nacp(OUTPUT "${NFSU2_NACP}"
        NAME "${NFSU2_SWITCH_APP_NAME}"
        AUTHOR "${NFSU2_SWITCH_AUTHOR}"
        VERSION "${NFSU2_SWITCH_VERSION}")
    if(NFSU2_SWITCH_ICON)
        nx_create_nro(${PROJECT_NAME} NACP "${NFSU2_NACP}" ICON "${NFSU2_SWITCH_ICON}")
    else()
        nx_create_nro(${PROJECT_NAME} NACP "${NFSU2_NACP}")
    endif()
endif()

# ── iOS app (arm64, MoltenVK + SDL2 static) ─────────────────────
#
# Built from the private artifact repo's ios-app workflow (kaskabayev/nfsu2-gen):
#   cmake -S . -B build -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
#     -DNFSU2_GEN_DIR=<lifted C> -DNFSU2_VULKAN=ON \
#     -DCMAKE_PREFIX_PATH=<SDL2+glslang install> -DMOLTENVK_FRAMEWORK=<MoltenVK.framework>
# The workflow packages the binary into NFSU2.app / an unsigned IPA.
if(IOS AND NOT NINTENDO_SWITCH)
    find_package(SDL2 REQUIRED)
    # SDL2 static on iOS: SDL2main carries the UIKit event pump's main(); our
    # main() is renamed through SDL_main.h (forced into every translation unit).
    target_include_directories(${PROJECT_NAME} PRIVATE
        $<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_options(${PROJECT_NAME} PRIVATE -include SDL_main.h)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2main)
    else()
        # Static-only install ships no SDL2main: the UIKit main() lives inside
        # libSDL2.a. Nothing references it until it loads, so pin the symbol:
        # -u _SDL_main pulls the archive member that defines main().
        target_link_options(${PROJECT_NAME} PRIVATE -Wl,-u,_SDL_main)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        SDL2::SDL2 "-framework UIKit" "-framework Metal" "-framework GameController"
        "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreMotion"
        "-framework CoreHaptics" "-framework Foundation" "-framework QuartzCore"
        "-framework CoreGraphics" "-framework OpenGLES" "-framework CoreVideo")
endif()

# The SetGammaRamp probe is a plain manual override now (sub_002F0D10 in
# recomp_manual.c; tools.recomp renames the generated body to _gen) -- no
# linker wrapping needed on any platform.
