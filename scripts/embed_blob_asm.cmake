# Embed a binary file into the extension via an assembler `.incbin` directive.
#
# Why not a C array: a ~55 MB shim as `{0x..,0x..,...}` builds a 55M-element
# initializer that OOM-kills cc1 on memory-constrained CI runners (observed on the
# manylinux arm64 build). `.incbin` copies the bytes at assemble time using
# negligible memory. Portable across ELF (Linux) and Mach-O (macOS) by branching on
# the symbol/section conventions.
#
# Invoked: cmake -DIN=<file> -DOUT=<file.S> -DSYM=<name> -DIS_APPLE=<0|1> -P embed_blob_asm.cmake
# Emits, matching the C decls `extern const unsigned char <SYM>[];` and
# `extern const unsigned long <SYM>_len;`.

if(NOT EXISTS "${IN}")
    message(FATAL_ERROR "embed_blob_asm: input '${IN}' does not exist")
endif()

file(SIZE "${IN}" _sz)

if(IS_APPLE)
    # Mach-O: C symbols get a leading underscore; use a const data section.
    set(_data "${SYM}")
    set(_len  "${SYM}_len")
    set(_g "_")            # underscore prefix
    set(_section ".section __TEXT,__const")
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
