# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
# Download only the expected immutable file. A supplied archive goes through
# the same checksum check as a downloaded archive.
if(LOCAL_FILE)
  set(archive "${LOCAL_FILE}")
else()
  set(archive "${DESTINATION}")
  if(NOT EXISTS "${archive}")
    file(DOWNLOAD "${URL}" "${archive}.partial"
      EXPECTED_HASH "SHA256=${SHA256}" TLS_VERIFY ON SHOW_PROGRESS)
    file(RENAME "${archive}.partial" "${archive}")
  endif()
endif()
file(SHA256 "${archive}" actual_hash)
string(TOLOWER "${SHA256}" expected_hash)
if(NOT actual_hash STREQUAL expected_hash)
  message(FATAL_ERROR "SHA256 mismatch for ${archive}. Expected ${expected_hash}, got ${actual_hash}")
endif()
if(LOCAL_FILE)
  file(COPY_FILE "${archive}" "${DESTINATION}")
endif()
