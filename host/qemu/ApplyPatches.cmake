# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

if(EXISTS "${SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_TOOL}" rev-parse HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE revision
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT revision STREQUAL "005c61c0ef6f6ae615283440b4bb7be51b2c6ddd")
        message(FATAL_ERROR "Unexpected QEMU source revision ${revision}")
    endif()
endif()

# Reverse any previously applied patches, starting from the last patch.
# The reverse checks also allow restarting a partially completed patch step.
set(reverse_series ${PATCHES})
list(REVERSE reverse_series)
foreach(patch IN LISTS reverse_series)
    execute_process(COMMAND "${GIT_TOOL}" apply --reverse --check "${PATCH_DIR}/${patch}"
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(applied EQUAL 0)
        execute_process(COMMAND "${GIT_TOOL}" apply --reverse --whitespace=nowarn "${PATCH_DIR}/${patch}"
            WORKING_DIRECTORY "${SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
endforeach()
foreach(patch IN LISTS PATCHES)
    execute_process(COMMAND "${GIT_TOOL}" apply --whitespace=nowarn "${PATCH_DIR}/${patch}"
        WORKING_DIRECTORY "${SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()

foreach(src IN LISTS PM_SOURCES)
    file(COPY "${src}" DESTINATION "${SOURCE_DIR}/hw/vfio")
endforeach()
if(NOT EXISTS "${SOURCE_DIR}/subprojects/dtc/libfdt/libfdt.h")
    file(CREATE_LINK "${DTC_DIR}" "${SOURCE_DIR}/subprojects/dtc" SYMBOLIC)
endif()
if(IS_DIRECTORY "${SUBPROJECTS}")
    foreach(name IN ITEMS keycodemapdb berkeley-softfloat-3 berkeley-testfloat-3)
        set(subproject "${SOURCE_DIR}/subprojects/${name}")
        if(NOT EXISTS "${subproject}/meson.build")
            file(COPY "${SUBPROJECTS}/${name}/" DESTINATION "${subproject}" PATTERN .git EXCLUDE)
            if(IS_DIRECTORY "${SOURCE_DIR}/subprojects/packagefiles/${name}")
                file(COPY "${SOURCE_DIR}/subprojects/packagefiles/${name}/" DESTINATION "${subproject}")
            endif()
        endif()
    endforeach()
endif()
