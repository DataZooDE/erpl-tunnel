# Minimal probe: can an RCDATA resource compiled into a STATIC library still be
# found with FindResource from the executable that links it?
#
# This matters because erpl_tunnel feeds the same blob sources to both the
# loadable extension (a DLL, where resources are certainly fine) and the static
# extension that ends up inside unittest.exe. If resources do not survive the
# static path, the shipped extension would work while every mesh SQL test failed
# — so we find out here rather than there.
cmake_minimum_required(VERSION 3.16)
project(rcdata_static C)
enable_language(RC)

# A payload with recognisable size, generated so the repo carries no binary.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/payload.bin" "")
foreach(i RANGE 63)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/payload.bin" "0123456789abcdef")
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/blob.rc" "${CMAKE_CURRENT_BINARY_DIR}/blob.rc" COPYONLY)

add_library(blobstatic STATIC lib.c "${CMAKE_CURRENT_BINARY_DIR}/blob.rc")
target_include_directories(blobstatic PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(rcdata_probe main.c)
target_link_libraries(rcdata_probe PRIVATE blobstatic)
