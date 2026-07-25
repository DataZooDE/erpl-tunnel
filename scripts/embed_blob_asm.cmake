# Embed a binary file into the extension via an assembler `.incbin` directive.
#
# Why not a C array: a ~55 MB shim as `{0x..,0x..,...}` builds a 55M-element
# initializer that OOM-kills cc1 on memory-constrained CI runners (observed on the
# manylinux arm64 build). `.incbin` copies the bytes at assemble time using
# negligible memory. Portable across ELF (Linux), Mach-O (macOS) and PE/COFF
# (Windows) by branching on the symbol/section conventions.
#
# On Windows this is also the reason we do NOT use an RCDATA resource, which would
# otherwise be the idiomatic choice: a resource does not survive being linked
# through a static library (nothing references it symbolically, so the linker never
# pulls that archive member in), which would silently break the static extension
# inside unittest.exe while the shipped DLL worked. An object file defines the
# symbols mesh_loader.cpp references, so the linker always pulls it in. Measured,
# see test/win/blob_embed.
#
# Invoked: cmake -DIN=<file> -DOUT=<file.S> -DSYM=<name> -DIS_APPLE=<0|1>
#                -DIS_WINDOWS=<0|1> -P embed_blob_asm.cmake
# Emits, matching the C decls `extern const unsigned char <SYM>[];` and
# `extern const uint64_t <SYM>_len;` (fixed width: `.quad` is 8 bytes, whereas
# `unsigned long` is only 4 under MSVC's LLP64).

if(NOT EXISTS "${IN}")
    message(FATAL_ERROR "embed_blob_asm: input '${IN}' does not exist")
endif()

file(SIZE "${IN}" _sz)

if(IS_APPLE)
    # Mach-O: C symbols get a leading underscore; use a const data section.
    set(_g "_")            # underscore prefix
    set(_section ".section __TEXT,__const")
elseif(IS_WINDOWS)
    # PE/COFF x86-64: no underscore prefix (unlike Mach-O, and unlike 32-bit PE);
    # read-only data lives in .rdata. Assembled by mingw's `as`, then handed to
    # MSVC's link.exe — safe because the object is pure data with no relocations,
    # no symbol references and no runtime-library directives.
    set(_g "")
    set(_section ".section .rdata,\"dr\"")
else()
    set(_g "")
    set(_section ".section .rodata")
endif()

file(WRITE "${OUT}"
"/* Auto-generated from ${IN} — do not edit. Embeds the file via .incbin. */
${_section}
.global ${_g}${SYM}
.balign 16
${_g}${SYM}:
.incbin \"${IN}\"
.global ${_g}${SYM}_len
.balign 8
${_g}${SYM}_len:
.quad ${_sz}
")
