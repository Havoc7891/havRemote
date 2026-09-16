# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 4.0)
project(BuildHelperFixture VERSION 1.0.0 LANGUAGES NONE)

set(FIXTURE_BINARY_DIR "${CMAKE_BINARY_DIR}")
set(FIXTURE_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
cmake_path(CONVERT "${HAVREMOTE_MINGW_ROOT}" TO_CMAKE_PATH_LIST FIXTURE_CACHED_ROOT NORMALIZE)
configure_file(stage.cmake.in "${CMAKE_BINARY_DIR}/stage.cmake" @ONLY)
set(STAGE configure)
include("${CMAKE_BINARY_DIR}/stage.cmake")

add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/built.txt"
  COMMAND "${CMAKE_COMMAND}" -DSTAGE=build -P "${CMAKE_BINARY_DIR}/stage.cmake"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/built.txt"
  VERBATIM)
add_custom_target(fixture ALL DEPENDS "${CMAKE_BINARY_DIR}/built.txt")
add_custom_target(package-symbols
  COMMAND "${CMAKE_COMMAND}" -DSTAGE=symbols -P "${CMAKE_BINARY_DIR}/stage.cmake"
  VERBATIM)
enable_testing()
add_test(NAME fixture
  COMMAND "${CMAKE_COMMAND}" -DSTAGE=test -P "${CMAKE_BINARY_DIR}/stage.cmake")

file(WRITE "${CMAKE_BINARY_DIR}/payload.txt" "Build helper fixture\n")
install(FILES "${CMAKE_BINARY_DIR}/payload.txt" DESTINATION .)
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME fixture)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/package.cmake")
file(WRITE "${CPACK_PROJECT_CONFIG_FILE}"
  "set(STAGE package)\ninclude(\"${CMAKE_BINARY_DIR}/stage.cmake\")\n")
include(CPack)
