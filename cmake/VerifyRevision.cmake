# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

if(EXPECTED)
    execute_process(COMMAND "${GIT}" -C "${SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT actual STREQUAL EXPECTED)
        message(FATAL_ERROR "Unexpected revision for ${SOURCE_DIR}. Expected ${EXPECTED}, found ${actual}.")
    endif()
endif()
