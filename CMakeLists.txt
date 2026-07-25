cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME erpl_tunnel)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Mesh backend bundle flag (ADR-012) -------------------------------------
# MESH_BACKEND selects which transport backends compile into the artifact:
#   ssh        SSH only (no Go, smallest, ~parity build)
#   tailscale  SSH + Tailscale shim
#   netbird    SSH + NetBird shim
#   both       SSH + both mesh shims (runtime picks one via the single-mesh latch)
# SSH is always present. Mesh shims are added in later milestones.
set(MESH_BACKEND "ssh" CACHE STRING "Which transport backends to bundle: ssh|tailscale|netbird|both")
message(STATUS "erpl_tunnel MESH_BACKEND=${MESH_BACKEND}")

# libssh2 (SSH transport) and OpenSSL (libssh2 crypto backend + PostHog telemetry
# HTTPS client) — both from vcpkg, static triplet → linked into the single artifact.
find_package(libssh2 CONFIG QUIET)
if(NOT libssh2_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH2 REQUIRED libssh2)
    set(LIBSSH2_TARGET ${LIBSSH2_LIBRARIES})
else()
    set(LIBSSH2_TARGET libssh2::libssh2)
endif()
find_package(OpenSSL REQUIRED)

include_directories(
    src/include
    posthog-telemetry/include
    duckdb/third_party/yyjson/include
    # httplib for the posthog-telemetry HTTPS client. Added directly (matching
    # erpl-idoc) rather than relying on posthog-telemetry's DUCKDB_SRC_INCLUDE gate,
    # which isn't set in every ci-tools flow (the v1.4-andium community build).
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib
)

# PostHog telemetry (same anonymous, opt-out telemetry as erpl / erpl-idoc).
set(DUCKDB_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party")
add_subdirectory(posthog-telemetry)

# Base sources — cross-platform (SSH via libssh2 + winsock on Windows). These build
# on every DuckDB platform.
set(EXTENSION_SOURCES
    src/tunnel_extension.cpp
    src/tunnel_connection.cpp
    src/tunnel_secret.cpp
    src/tunnel_manager.cpp
    src/pragma_tunnel_create.cpp
    src/pragma_tunnel_close.cpp
    src/pragma_tunnel_close_all.cpp
    src/scanner_tunnels.cpp
    src/pragma_tunnel_export.cpp
    src/ssh_exporter.cpp
)

# --- Mesh backends: Unix-only (Go c-shared shims use socketpair/AF_UNIX + dlopen) --
# The mesh C++ (mesh_loader/backend/forwarder/peers_json) and the Go shims compile
# only on glibc Linux + macOS. On Windows/musl the extension is SSH-only, and none of
# the mesh code is compiled — guarded by ERPL_TUNNEL_HAS_MESH in the base sources.
set(MESH_BLOB_SOURCES "")
set(MESH_DEFINES "")

set(MESH_WANTED OFF)
if(MESH_BACKEND STREQUAL "tailscale" OR MESH_BACKEND STREQUAL "netbird" OR MESH_BACKEND STREQUAL "both")
    set(MESH_WANTED ON)
endif()

# On Windows cgo needs a mingw-w64 gcc. Resolve it before committing to a mesh
# build: without one this degrades to SSH-only rather than failing, so third-party
# builders still get a working extension (our own CI sets ERPL_REQUIRE_MESH so a
# missing toolchain is loud there).
if(MESH_WANTED AND WIN32)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bootstrap_mingw.cmake)
endif()

if(MESH_WANTED)
    list(APPEND EXTENSION_SOURCES
        src/mesh_loader.cpp
        src/mesh_backend.cpp
        src/mesh_forwarder.cpp
        src/mesh_exporter.cpp
        src/mesh_peers_json.cpp
        duckdb/third_party/yyjson/yyjson.cpp)
    list(APPEND MESH_DEFINES ERPL_TUNNEL_HAS_MESH)
    # Bootstrap a new-enough Go (system Go if adequate, else a pinned download).
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bootstrap_go.cmake)
endif()

# Embed the shim blobs via assembler .incbin (a 55 MB C array OOM-kills cc1 on
# constrained CI runners). Needs the ASM language.
# MSVC has no GNU assembler, so on Windows the .S is assembled to a COFF object by
# mingw (which the Go build needs anyway) and handed to link.exe as a plain object.
# That is safe because it is pure data: no relocations, no symbol references, no
# runtime-library directives. An RCDATA resource — the otherwise idiomatic Windows
# answer — was measured and rejected: a resource does not survive being linked
# through a static library, which would have broken the static extension inside
# unittest.exe while the shipped DLL worked. See test/win/blob_embed.
if(MESH_WANTED AND NOT WIN32)
    enable_language(ASM)
endif()

function(add_mesh_shim MESH_ID SHIM_DIR SYM DEFINE)
    if(WIN32)
        set(SHIM_LIB "${CMAKE_CURRENT_BINARY_DIR}/${MESH_ID}_shim.dll")
    else()
        set(SHIM_LIB "${CMAKE_CURRENT_BINARY_DIR}/${MESH_ID}_shim.so")
    endif()
    set(BLOB_S "${CMAKE_CURRENT_BINARY_DIR}/${MESH_ID}_shim_blob.S")
    file(GLOB SHIM_GO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/${SHIM_DIR}/*.go")
    # The shared stream-pair module is a source dependency of both shims.
    file(GLOB PAIR_GO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/shim/meshpair/*.go")

    # GOTOOLCHAIN=local: use exactly the bootstrapped Go (already >= the shims' go.mod
    # requirement), never fetch — deterministic and offline-friendly in CI. GOROOT is
    # pinned so a stale environment GOROOT can't mis-root the toolchain.
    set(_go_env CGO_ENABLED=1 GOFLAGS=-mod=mod GOTOOLCHAIN=local "GOROOT=${GO_ROOT}")
    if(WIN32)
        # cgo cannot use MSVC; it needs a gcc-style driver. -static-libgcc keeps the
        # shim self-contained, which matters because it is extracted ALONE into a
        # cache directory at runtime and must not need libgcc beside it.
        list(APPEND _go_env "CC=${MINGW_CC}" "CGO_LDFLAGS=-static-libgcc")
    endif()

    add_custom_command(
        OUTPUT ${SHIM_LIB}
        COMMAND ${CMAKE_COMMAND} -E env ${_go_env}
                ${GO_EXECUTABLE} build -buildmode=c-shared -o ${SHIM_LIB} .
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${SHIM_DIR}
        DEPENDS ${SHIM_GO_SRC} ${PAIR_GO_SRC}
                ${CMAKE_CURRENT_SOURCE_DIR}/${SHIM_DIR}/go.mod
                ${CMAKE_CURRENT_SOURCE_DIR}/${SHIM_DIR}/go.sum
        COMMENT "Building ${MESH_ID} Go c-shared shim"
        VERBATIM)

    if(APPLE)
        set(_is_apple 1)
    else()
        set(_is_apple 0)
    endif()
    if(WIN32)
        set(_is_windows 1)
    else()
        set(_is_windows 0)
    endif()
    add_custom_command(
        OUTPUT ${BLOB_S}
        COMMAND ${CMAKE_COMMAND} -DIN=${SHIM_LIB} -DOUT=${BLOB_S} -DSYM=${SYM}
                -DIS_APPLE=${_is_apple} -DIS_WINDOWS=${_is_windows}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_blob_asm.cmake
        DEPENDS ${SHIM_LIB}
        COMMENT "Embedding ${MESH_ID} shim blob (.incbin)"
        VERBATIM)

    if(WIN32)
        # Assemble here rather than letting CMake drive it: enable_language(ASM)
        # under MSVC selects MASM, which has no .incbin at all.
        set(BLOB_OBJ "${CMAKE_CURRENT_BINARY_DIR}/${MESH_ID}_shim_blob.obj")
        add_custom_command(
            OUTPUT ${BLOB_OBJ}
            COMMAND "${MINGW_CC}" -c ${BLOB_S} -o ${BLOB_OBJ}
            DEPENDS ${BLOB_S} ${SHIM_LIB}
            COMMENT "Assembling ${MESH_ID} shim blob into a COFF object"
            VERBATIM)
        set_source_files_properties(${BLOB_OBJ} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        set(MESH_BLOB_SOURCES ${MESH_BLOB_SOURCES} ${BLOB_OBJ} PARENT_SCOPE)
    else()
        # The .S embeds SHIM_LIB via .incbin at assemble time; re-assemble if it changes.
        set_source_files_properties(${BLOB_S} PROPERTIES OBJECT_DEPENDS ${SHIM_LIB})
        set(MESH_BLOB_SOURCES ${MESH_BLOB_SOURCES} ${BLOB_S} PARENT_SCOPE)
    endif()
    set(MESH_DEFINES ${MESH_DEFINES} ${DEFINE} PARENT_SCOPE)
endfunction()

if(MESH_BACKEND STREQUAL "tailscale" OR MESH_BACKEND STREQUAL "both")
    add_mesh_shim(ts shim/ts ts_shim_blob MESH_HAVE_TAILSCALE)
endif()
if(MESH_BACKEND STREQUAL "netbird" OR MESH_BACKEND STREQUAL "both")
    add_mesh_shim(nb shim/nb nb_shim_blob MESH_HAVE_NETBIRD)
endif()

list(APPEND EXTENSION_SOURCES ${MESH_BLOB_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Force C++17 on the extension objects (match DuckDB core).
set_target_properties(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
                      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# GCC 14+ emits STB_GNU_UNIQUE ('u') defs for ODR-used DuckDB class statics, which
# collide with core's strong defs at link (fails duckdb's own tools/plan_serializer).
# -fno-gnu-unique turns ours into weak COMDAT defs the strong core def overrides.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${EXTENSION_NAME} PRIVATE -fno-gnu-unique)
    target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -fno-gnu-unique)
endif()

target_link_libraries(${EXTENSION_NAME} ${LIBSSH2_TARGET} posthog_telemetry OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${LIBSSH2_TARGET} posthog_telemetry OpenSSL::SSL OpenSSL::Crypto)

# Per-build mesh backend defines (MESH_HAVE_TAILSCALE / MESH_HAVE_NETBIRD) and the
# dynamic loader (dlopen/dlsym) used to lazily activate a mesh shim at runtime.
foreach(def ${MESH_DEFINES})
    target_compile_definitions(${EXTENSION_NAME} PRIVATE ${def})
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE ${def})
endforeach()
if(UNIX AND NOT APPLE)
    target_link_libraries(${EXTENSION_NAME} ${CMAKE_DL_LIBS})
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    target_link_libraries(${EXTENSION_NAME} ws2_32)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ws2_32)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
