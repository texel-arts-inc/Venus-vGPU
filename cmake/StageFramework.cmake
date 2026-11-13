# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set(version "${FRAMEWORK}/Versions/A")
set(resources "${version}/Resources")
file(MAKE_DIRECTORY "${resources}/icd.d")
file(COPY_FILE "${ICD}" "${version}/Venus")
execute_process(
    COMMAND /usr/bin/install_name_tool -id @rpath/Venus.framework/Versions/A/Venus "${version}/Venus"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(COMMAND /usr/bin/otool -L "${version}/Venus"
    OUTPUT_VARIABLE linkage COMMAND_ERROR_IS_FATAL ANY)
string(REGEX MATCHALL "\n[ \t]+[^ \n]+" dependencies "${linkage}")
foreach(dependency IN LISTS dependencies)
    string(STRIP "${dependency}" dependency)
    if(NOT dependency MATCHES "^(/usr/lib/|/System/Library/|@rpath/Venus[.]framework/)")
        message(FATAL_ERROR "Unexpected Venus framework dependency: ${dependency}")
    endif()
endforeach()
file(WRITE "${resources}/Info.plist" [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Venus</string>
<key>CFBundleIdentifier</key><string>com.texel-arts.venus-vgpu.framework</string>
<key>CFBundleName</key><string>Venus</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
</dict></plist>
]=])
file(WRITE "${resources}/icd.d/Venus_icd.json" [=[{
  "file_format_version": "1.0.1",
  "ICD": {
    "api_version": "1.4.363",
    "library_arch": "64",
    "library_path": "../../Venus"
  }
}
]=])
file(CREATE_LINK A "${FRAMEWORK}/Versions/Current" SYMBOLIC)
file(CREATE_LINK Versions/Current/Resources "${FRAMEWORK}/Resources" SYMBOLIC)
file(CREATE_LINK Versions/Current/Venus "${FRAMEWORK}/Venus" SYMBOLIC)
execute_process(
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none "${FRAMEWORK}"
    COMMAND_ERROR_IS_FATAL ANY
)
