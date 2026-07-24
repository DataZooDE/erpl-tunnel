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
#   glibc Linux + macOS -> 'both' (ssh + tailscale + netbird; runtime picks a mesh
#                          via the single-mesh latch; mesh shims dlopen'd lazily).
#   Windows + musl       -> 'ssh'  (the Go c-shared mesh shims use socketpair/AF_UNIX
#                          + dlopen, which don't exist there; SSH via libssh2 works).
# One extension name, per-platform backend set. Override with `MESH_BACKEND=... make`.
if(DEFINED ENV{MESH_BACKEND})
    set(_erpl_mesh "$ENV{MESH_BACKEND}")
else()
    set(_erpl_mesh "both")
    if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "Windows")
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
