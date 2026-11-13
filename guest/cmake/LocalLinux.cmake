# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Snapshot exactly the pinned commit into an independent worktree.
execute_process(COMMAND "${GIT}" -C "${LOCAL_SOURCE}" rev-parse "HEAD"
  OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT}" -C "${LOCAL_SOURCE}" status --porcelain
  OUTPUT_VARIABLE status OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT head STREQUAL COMMIT OR status)
  message(FATAL_ERROR "VENUS_GUEST_LINUX_SOURCE must be a clean checkout at ${COMMIT}")
endif()
file(REMOVE_RECURSE "${DESTINATION}")
file(MAKE_DIRECTORY "${DESTINATION}")
set(archive "${DESTINATION}/.venus-linux-source.tar")
execute_process(COMMAND "${GIT}" -C "${LOCAL_SOURCE}" archive --format=tar
  "--output=${archive}" "${COMMIT}" COMMAND_ERROR_IS_FATAL ANY)
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${DESTINATION}")
file(REMOVE "${archive}")
