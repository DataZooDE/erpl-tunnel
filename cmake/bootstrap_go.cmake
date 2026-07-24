# bootstrap_go.cmake — provide a Go toolchain new enough to build the mesh shims,
# independent of what the build environment ships.
#
# Why: the DuckDB community-extensions CI installs Go 1.20.5 in its manylinux linux
# container (1.23 on the macOS/Windows runners). Our shims need a much newer Go
# (tsnet ≥ 1.24, netbird go.mod `go 1.25.5`), and Go 1.20 predates the GOTOOLCHAIN
# auto-download mechanism, so it cannot self-upgrade. We therefore use the system Go
# if (and only if) it is new enough, and otherwise download a pinned Go SDK for the
# build host's OS/arch and use that. Deterministic: GOTOOLCHAIN=local, no surprise
# network fetches once a suitable toolchain is in hand.
#
# Sets in the parent scope:  GO_EXECUTABLE  (absolute path to a >= ERPL_GO_MIN `go`)

# Minimum + pinned Go. Must satisfy the HIGHEST `go` directive across the shims:
# shim/ts/go.mod is `go 1.26.4` (tailscale), shim/nb/go.mod is `go 1.25.5` (netbird).
# GOTOOLCHAIN=local means the toolchain never auto-upgrades, so it must already be >=.
set(ERPL_GO_MIN "1.26.4")     # minimum that satisfies the shims' go.mod `go` lines
set(ERPL_GO_PIN "1.26.4")     # version downloaded when the system Go is too old

# Pinned SHA-256 of the go${ERPL_GO_PIN} SDK tarballs (from https://go.dev/dl) so the
# published toolchain is not mutable at download time. Update when ERPL_GO_PIN bumps.
set(_go_sha_linux_amd64  "1153d3d50e0ac764b447adfe05c2bcf08e889d42a02e0fe0259bd47f6733ad7f")
set(_go_sha_linux_arm64  "ef758ae7c6cf9267c9c0ef080b8965f453d89ab2d25d9eb22de4405925238768")
set(_go_sha_darwin_amd64 "05dc9b5f9997744520aaebb3d5deaa7c755371aebbfb7f97c2511a9f3367538d")
set(_go_sha_darwin_arm64 "b62ad2b6d7d2464f12a5bcad7ff47f19d08325773b5efd21610e445a05a9bf53")

# --- map host OS/arch to Go's release naming --------------------------------
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(_go_os "linux")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(_go_os "darwin")
else()
    message(FATAL_ERROR "erpl_tunnel mesh backends are Linux/macOS only (host: ${CMAKE_HOST_SYSTEM_NAME}).")
endif()

set(_go_arch_raw "${CMAKE_HOST_SYSTEM_PROCESSOR}")
if(NOT _go_arch_raw)
    # CMAKE_HOST_SYSTEM_PROCESSOR is empty in `cmake -P` script mode; fall back to uname.
    execute_process(COMMAND uname -m OUTPUT_VARIABLE _go_arch_raw OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(_go_arch_raw MATCHES "^(x86_64|amd64|AMD64)$")
    set(_go_arch "amd64")
elseif(_go_arch_raw MATCHES "^(aarch64|arm64|ARM64)$")
    set(_go_arch "arm64")
else()
    message(FATAL_ERROR "Unsupported host arch for the Go mesh shims: ${_go_arch_raw}")
endif()

# --- helper: is `${candidate}` a go >= ERPL_GO_MIN? -------------------------
function(_erpl_go_is_new_enough candidate out_var)
    set(${out_var} OFF PARENT_SCOPE)
    if(NOT candidate OR NOT EXISTS "${candidate}")
        return()
    endif()
    execute_process(COMMAND "${candidate}" version
                    OUTPUT_VARIABLE _ver_out ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        return()
    endif()
    # "go version go1.26.5 linux/amd64" -> 1.26.5
    string(REGEX MATCH "go([0-9]+\\.[0-9]+(\\.[0-9]+)?)" _m "${_ver_out}")
    if(NOT CMAKE_MATCH_1)
        return()
    endif()
    if(NOT CMAKE_MATCH_1 VERSION_LESS ERPL_GO_MIN)
        set(${out_var} ON PARENT_SCOPE)
    endif()
endfunction()

# --- 1) prefer a system Go that is new enough (fast, offline) ---------------
set(GO_EXECUTABLE "")
set(GO_ROOT "")
if(NOT DEFINED ERPL_FORCE_GO_DOWNLOAD OR NOT ERPL_FORCE_GO_DOWNLOAD)
    find_program(_sys_go go)
    if(_sys_go)
        _erpl_go_is_new_enough("${_sys_go}" _sys_ok)
        if(_sys_ok)
            set(GO_EXECUTABLE "${_sys_go}")
            # Pin GOROOT to this Go's own root so a stale GOROOT in the environment
            # (e.g. the manylinux Go 1.20.5 at /usr/local/go) cannot poison it.
            execute_process(COMMAND "${_sys_go}" env GOROOT
                            OUTPUT_VARIABLE GO_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            message(STATUS "erpl_tunnel: using system Go (${_sys_go}, >= ${ERPL_GO_MIN}, GOROOT=${GO_ROOT})")
        else()
            message(STATUS "erpl_tunnel: system Go too old (< ${ERPL_GO_MIN}); will download go${ERPL_GO_PIN}")
        endif()
    endif()
endif()

# --- 2) otherwise download a pinned, checksum-verified Go SDK (cached) -------
if(NOT GO_EXECUTABLE)
    set(_go_root "${CMAKE_BINARY_DIR}/_go_sdk/go${ERPL_GO_PIN}")
    set(_go_bin "${_go_root}/go/bin/go")
    if(NOT EXISTS "${_go_bin}")
        set(_tar "go${ERPL_GO_PIN}.${_go_os}-${_go_arch}.tar.gz")
        set(_url "https://go.dev/dl/${_tar}")
        set(_dl "${CMAKE_BINARY_DIR}/_go_sdk/${_tar}")
        set(_sha "${_go_sha_${_go_os}_${_go_arch}}")
        if(NOT _sha)
            message(FATAL_ERROR "No pinned SHA-256 for go${ERPL_GO_PIN} ${_go_os}-${_go_arch}; add it to bootstrap_go.cmake.")
        endif()
        message(STATUS "erpl_tunnel: downloading ${_url}")
        file(DOWNLOAD "${_url}" "${_dl}" SHOW_PROGRESS STATUS _dlst TLS_VERIFY ON
             EXPECTED_HASH "SHA256=${_sha}")
        list(GET _dlst 0 _dlrc)
        if(NOT _dlrc EQUAL 0)
            list(GET _dlst 1 _dlmsg)
            message(FATAL_ERROR "Failed to download/verify Go SDK: ${_dlmsg} (${_url})")
        endif()
        file(MAKE_DIRECTORY "${_go_root}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${_dl}"
                        WORKING_DIRECTORY "${_go_root}" RESULT_VARIABLE _xrc)
        if(NOT _xrc EQUAL 0)
            message(FATAL_ERROR "Failed to extract Go SDK: ${_dl}")
        endif()
        file(REMOVE "${_dl}")
    endif()
    if(NOT EXISTS "${_go_bin}")
        message(FATAL_ERROR "Go bootstrap failed: ${_go_bin} not present after download.")
    endif()
    set(GO_EXECUTABLE "${_go_bin}")
    set(GO_ROOT "${_go_root}/go")
    message(STATUS "erpl_tunnel: using bootstrapped Go (${_go_bin}, GOROOT=${GO_ROOT})")
endif()

set(GO_EXECUTABLE "${GO_EXECUTABLE}" PARENT_SCOPE)
set(GO_ROOT "${GO_ROOT}" PARENT_SCOPE)
