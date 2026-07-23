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
)

# PostHog telemetry (same anonymous, opt-out telemetry as erpl / erpl-idoc).
set(DUCKDB_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party")
add_subdirectory(posthog-telemetry)

set(EXTENSION_SOURCES
    src/tunnel_extension.cpp
    src/tunnel_connection.cpp
    src/tunnel_secret.cpp
    src/tunnel_manager.cpp
    src/pragma_tunnel_create.cpp
    src/pragma_tunnel_close.cpp
    src/pragma_tunnel_close_all.cpp
    src/scanner_tunnels.cpp
)

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

if(WIN32)
    target_link_libraries(${EXTENSION_NAME} ws2_32)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ws2_32)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
