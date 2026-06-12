# =============================================================================
# DinoPark — static recompilation of DinoPark Tycoon (1993, Manley/MECC)
#
# Built with the pcrecomp toolchain. The original is a 16-bit DOS program, so the
# native target is 32-bit/Win32 (pointer math + low VAs line up). Scaffold only —
# the recompiled sources land as Phase 1 progresses.
#
# Build:  cmake -B build -G "Visual Studio 17 2022" -A Win32
#         cmake --build build --config Release
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(DinoPark VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)

if(MSVC)
    add_link_options(/DYNAMICBASE:NO)        # recomp code uses fixed VAs
    add_link_options(/STACK:8388608)         # 8 MB — deep mechanical call graphs
    add_compile_options(/W1)
    add_compile_options(/wd4996 /wd4244 /wd4146 /wd4102 /wd4013 /wd4101 /wd4267 /wd4018)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /Oi")
endif()

# Hand-written runtime / support sources (grows as we shim DOS/VGA/Miles).
set(RUNTIME_SOURCES
    src/main.c
)

# Generated recompiled code — auto-discovered once the lifter runs.
file(GLOB GENERATED_SOURCES "src/recomp/gen/recomp_*.c")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/recomp_dispatch.c")
    list(APPEND GENERATED_SOURCES "src/recomp/gen/recomp_dispatch.c")
endif()

add_executable(dinopark ${RUNTIME_SOURCES} ${GENERATED_SOURCES})

target_include_directories(dinopark PRIVATE
    src
    src/recomp
    src/recomp/gen
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "Building 64-bit! 16-bit-derived recomp wants 32-bit: cmake -A Win32")
endif()
