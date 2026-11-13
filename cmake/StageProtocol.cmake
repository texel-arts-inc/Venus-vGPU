# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

file(COPY "${PROTOCOL}/" DESTINATION "${MESA}/subprojects/venus-protocol-1.1.3"
    PATTERN ".git" EXCLUDE)
