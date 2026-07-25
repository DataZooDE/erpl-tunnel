# Probe: can a large binary blob be embedded on Windows in a way that survives
# being linked through a STATIC library, and can MSVC's link.exe consume an object
# produced by mingw's assembler?
#
# Why this exists: erpl_tunnel feeds the same blob source to both the loadable
# extension (a DLL) and the static extension that ends up inside unittest.exe. An
# RCDATA resource was tried first and does NOT survive the static path — nothing
# references the .res symbolically, so the linker never pulls it in, and the result
# is a silent asymmetry where the shipped DLL works but every mesh SQL test fails.
#
# This approach instead assembles the blob into a COFF object with .incbin, exactly
# as the Unix build already does for ELF/Mach-O. The object defines the symbols
# mesh_loader.cpp references, so the linker has a reason to pull it in — which is
# precisely what an RCDATA resource lacks.
cmake_minimum_required(VERSION 3.16)
project(blob_embed C)

if(NOT MINGW_CC)
    message(FATAL_ERROR "pass -DMINGW_CC=<path to mingw gcc>")
endif()

set(PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/payload.bin")
file(WRITE "${PAYLOAD}" "")
foreach(i RANGE 63)
    file(APPEND "${PAYLOAD}" "0123456789abcdef")   # 64 * 16 = 1024 bytes
endforeach()
file(SIZE "${PAYLOAD}" PAYLOAD_SIZE)

# PE/COFF x86-64: C symbols are NOT underscore-prefixed (unlike Mach-O, and unlike
# 32-bit PE), and read-only data lives in .rdata.
set(BLOB_S "${CMAKE_CURRENT_BINARY_DIR}/blob.S")
file(WRITE "${BLOB_S}"
"/* Auto-generated probe blob. */
.section .rdata,\"dr\"
.global probe_blob
.balign 16
probe_blob:
.incbin \"${PAYLOAD}\"
.global probe_blob_len
.balign 8
probe_blob_len:
.quad ${PAYLOAD_SIZE}
")

set(BLOB_OBJ "${CMAKE_CURRENT_BINARY_DIR}/blob.obj")
add_custom_command(OUTPUT "${BLOB_OBJ}"
    COMMAND "${MINGW_CC}" -c "${BLOB_S}" -o "${BLOB_OBJ}"
    DEPENDS "${BLOB_S}" "${PAYLOAD}"
    COMMENT "Assembling probe blob into a COFF object with mingw"
    VERBATIM)
add_custom_target(blob_obj DEPENDS "${BLOB_OBJ}")
set_source_files_properties("${BLOB_OBJ}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_library(blobstatic STATIC lib.c "${BLOB_OBJ}")
add_dependencies(blobstatic blob_obj)

add_executable(blob_probe main.c)
target_link_libraries(blob_probe PRIVATE blobstatic)
