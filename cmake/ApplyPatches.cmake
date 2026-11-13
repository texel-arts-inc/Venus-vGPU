# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(patch IN LISTS PATCH_FILES)
    # ExternalProject can repeat a patch step when its CMake command changes.
    execute_process(
        COMMAND "${GIT}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE already_applied
    )
    if(NOT already_applied EQUAL 0)
        execute_process(
            COMMAND "${GIT}" apply "${patch}"
            WORKING_DIRECTORY "${SOURCE_DIR}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endforeach()
