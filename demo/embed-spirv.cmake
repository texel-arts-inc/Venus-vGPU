# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

file(READ "${INPUT}" bytes HEX)
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1,\n    " words "${bytes}")
file(WRITE "${OUTPUT}" "#pragma once\n#include <cstdint>\n\ninline constexpr std::uint32_t ${SYMBOL}[] = {\n    ${words}\n};\n")
