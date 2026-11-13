# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# CMake's executable finder is find_program. Keep tool discovery distinct from find_package.
macro(find_executable)
    find_program(${ARGV})
endmacro()
