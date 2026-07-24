# This file is included by DuckDB's build system. It specifies which extension to load.

# GCC 14+ (the rolling ci-tools Linux image) emits STB_GNU_UNIQUE defs for ODR-used
# DuckDB class statics, which collide with core's strong defs at link (fails duckdb's
# own tools/plan_serializer). Apply -fno-gnu-unique globally on GCC so they become weak
# COMDAT defs. Kept here (not only in MainDistributionPipeline.yml) so builders that
# don't pass extra_extension_config — notably duckdb/community-extensions — also get it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-gnu-unique")
endif()

# Mesh backend bundle selection (ADR-012). The PUBLISHED artifact (community registry
# and self-hosted) is 'both' — one erpl_tunnel carrying ssh + tailscale + netbird, the
# runtime picking one mesh via the single-mesh latch. The mesh shims are still only
# dlopen'd lazily, so SSH-only sessions map no Go even from a 'both' build. Local devs
# who want a fast, Go-free iteration build override with `MESH_BACKEND=ssh make debug`.
if(DEFINED ENV{MESH_BACKEND})
    set(MESH_BACKEND "$ENV{MESH_BACKEND}" CACHE STRING "mesh backends to bundle" FORCE)
else()
    set(MESH_BACKEND "both" CACHE STRING "mesh backends to bundle" FORCE)
endif()

# Extension from this repo
duckdb_extension_load(erpl_tunnel
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
