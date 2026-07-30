# Locate a cgo-capable mingw-w64 gcc on Windows and expose it as MINGW_CC.
#
# Why this has to live in CMake rather than in a CI step: the build that matters
# most — duckdb/community-extensions — compiles this extension on infrastructure we
# do not control and drives it only through extension_config.cmake. A "install
# mingw first" instruction in our own workflow would not reach it. So the mingw
# dependency is resolved the same way the Go toolchain already is.
#
# cgo cannot use MSVC: it drives a gcc/clang-style compiler and expects objects the
# Go linker understands. The extension itself stays MSVC — the two only ever meet
# over a plain C ABI and OS handles, which is why mixing them is safe here.

set(MINGW_CC "" CACHE FILEPATH "mingw-w64 gcc used to build the Go mesh shims")

if(NOT WIN32)
    return()
endif()

# 1) explicit override always wins
if(DEFINED ENV{ERPL_MINGW_CC} AND EXISTS "$ENV{ERPL_MINGW_CC}")
    set(MINGW_CC "$ENV{ERPL_MINGW_CC}" CACHE FILEPATH "" FORCE)
endif()

# 2) search PATH and the usual install locations
if(NOT MINGW_CC)
    find_program(_erpl_mingw_candidate
        NAMES x86_64-w64-mingw32-gcc gcc
        PATHS
            "C:/msys64/mingw64/bin"
            "C:/mingw64/bin"
            "C:/ProgramData/chocolatey/lib/mingw/tools/install/mingw64/bin"
            "$ENV{RTOOLS44_HOME}/x86_64-w64-mingw32.static.posix/bin"
        )
    if(_erpl_mingw_candidate)
        # Verify it really is mingw-w64 and not, say, clang-cl or an MSYS (non-mingw)
        # gcc — both would produce objects the Go linker cannot use, and the failure
        # would appear much later and far less legibly.
        execute_process(COMMAND "${_erpl_mingw_candidate}" -dumpmachine
                        OUTPUT_VARIABLE _erpl_triple OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE _erpl_rc)
        if(_erpl_rc EQUAL 0 AND _erpl_triple MATCHES "mingw")
            set(MINGW_CC "${_erpl_mingw_candidate}" CACHE FILEPATH "" FORCE)
            message(STATUS "erpl_tunnel: using mingw-w64 gcc ${MINGW_CC} (${_erpl_triple})")
        else()
            message(STATUS "erpl_tunnel: ignoring ${_erpl_mingw_candidate} — not a mingw toolchain (${_erpl_triple})")
        endif()
    endif()
endif()

# 3) no mingw: stop by default, because MESH_BACKEND asked for mesh backends and we
#    cannot produce them. Degrading here silently ships an extension whose
#    tunnel_peers/tunnel_self simply do not exist, which is far harder to diagnose
#    than a build error naming the missing toolchain.
if(NOT MINGW_CC)
    if(ERPL_REQUIRE_MESH)
        message(FATAL_ERROR
            "erpl_tunnel: MESH_BACKEND=${MESH_BACKEND} needs a mingw-w64 gcc for cgo on Windows, "
            "and none was found.\n"
            "  * install one:            msys2 + mingw-w64-x86_64-gcc, or set ERPL_MINGW_CC\n"
            "  * or build SSH-only:      MESH_BACKEND=ssh\n"
            "  * or accept a downgrade:  -DERPL_REQUIRE_MESH=OFF")
    endif()
    message(WARNING
        "erpl_tunnel: no mingw-w64 gcc found — building SSH-only on this Windows host "
        "because ERPL_REQUIRE_MESH=OFF was set. tunnel_peers/tunnel_self will NOT exist "
        "in this artifact.")
    set(MESH_BACKEND "ssh" CACHE STRING "mesh backends to bundle" FORCE)
    # include() runs in the caller's scope, so a plain set() is what propagates here.
    set(MESH_WANTED OFF)
endif()
