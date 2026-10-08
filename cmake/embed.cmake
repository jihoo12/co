# Usage: cmake -DIN=<file> -DOUT=<file.cpp> -DNAME=<symbol> -P embed.cmake
# Writes a C++ file defining `co::<NAME>` (the bytes of IN) and `co::<NAME>Size`.
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}" "// Generated from ${IN}; do not edit.
#include <cstddef>
namespace co {
alignas(8) extern const unsigned char ${NAME}[] = {
${bytes}};
extern const std::size_t ${NAME}Size = sizeof(${NAME});
} // namespace co
")
