# Portable blob embedder — turn a binary file into a C source defining
#   const unsigned char <SYM>[]; const unsigned long <SYM>_len;
# Invoked at build time: cmake -DIN=<file> -DOUT=<file.c> -DSYM=<name> -P embed_blob.cmake
# Pure CMake (no xxd/objcopy) so it works identically on Linux and macOS.

if(NOT EXISTS "${IN}")
    message(FATAL_ERROR "embed_blob: input '${IN}' does not exist")
endif()

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR nbytes "${hexlen} / 2")

# Turn "aabbcc..." into "0xaa,0xbb,0xcc,..." with line wrapping.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
# Wrap every 20 bytes for readability / to avoid absurdly long lines.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){20})" "\\1\n" body "${body}")

file(WRITE "${OUT}" "/* Auto-generated from ${IN} — do not edit. */\n")
file(APPEND "${OUT}" "#include <stddef.h>\n")
file(APPEND "${OUT}" "const unsigned char ${SYM}[] = {\n${body}\n};\n")
file(APPEND "${OUT}" "const unsigned long ${SYM}_len = ${nbytes}UL;\n")
