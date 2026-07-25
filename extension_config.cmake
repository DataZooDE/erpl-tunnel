# This file is included by DuckDB's build system. It specifies which extension to load.

# GCC 14+ (the rolling ci-tools Linux image) emits STB_GNU_UNIQUE defs for ODR-used
# DuckDB class statics, which collide with core's strong defs at link (fails duckdb's
# own tools/plan_serializer). Apply -fno-gnu-unique globally on GCC so they become weak
# COMDAT defs. Kept here (not only in MainDistributionPipeline.yml) so builders that
# don't pass extra_extension_config — notably duckdb/community-extensions — also get it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-gnu-unique")
endif()

# Mesh backend bundle selection (ADR-012), per platform:
#   glibc Linux + macOS + Windows -> 'both' (ssh + tailscale + netbird; the runtime
#                          picks one mesh via the single-mesh latch and loads its
#                          shim lazily).
#   musl / wasm          -> 'ssh'  (no Go cgo c-shared there).
#
# Windows carried 'ssh' until the stream pair stopped depending on AF_UNIX
# socketpair (shim/meshpair) and the loader learned LoadLibraryW. It still degrades
# to SSH-only automatically if no mingw-w64 gcc is available for cgo — see
# cmake/bootstrap_mingw.cmake. Set ERPL_DISABLE_WINDOWS_MESH to force that.
# One extension name, per-platform backend set. Override with `MESH_BACKEND=... make`.
if(DEFINED ENV{MESH_BACKEND})
    set(_erpl_mesh "$ENV{MESH_BACKEND}")
else()
    set(_erpl_mesh "both")
    if((WIN32 OR CMAKE_SYSTEM_NAME MATCHES "Windows") AND DEFINED ENV{ERPL_DISABLE_WINDOWS_MESH})
        set(_erpl_mesh "ssh")
    endif()
    # musl / wasm targets have no Go cgo c-shared — SSH-only there too.
    if("$ENV{DUCKDB_PLATFORM}" MATCHES "musl|wasm" OR "${DUCKDB_PLATFORM}" MATCHES "musl|wasm")
        set(_erpl_mesh "ssh")
    endif()
endif()
set(MESH_BACKEND "${_erpl_mesh}" CACHE STRING "mesh backends to bundle" FORCE)
message(STATUS "erpl_tunnel: MESH_BACKEND=${MESH_BACKEND} (platform=${CMAKE_SYSTEM_NAME}/$ENV{DUCKDB_PLATFORM})")

# Extension from this repo
duckdb_extension_load(erpl_tunnel
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
