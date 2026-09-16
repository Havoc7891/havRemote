# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

function(havremote_enable_unix_packaging target)
  if(WIN32 OR NOT UNIX OR NOT HAVREMOTE_BUILD_APP OR HAVREMOTE_CONFIGURE_ONLY)
    return()
  endif()
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Unknown packaging target: ${target}")
  endif()

  # Resources stay beside the executable, matching the build-tree lookup paths.
  # Only project-built shared libraries are bundled. GTK, libsecret, OpenSSL,
  # the C/C++ runtime, and other operating-system dependencies stay external.
  if(APPLE)
    set(_executable_rpath "@loader_path/lib")
    set(_library_rpath "@loader_path")
  else()
    set(_executable_rpath "$ORIGIN/lib")
    set(_library_rpath "$ORIGIN")
  endif()
  set_target_properties("${target}" PROPERTIES
    INSTALL_RPATH "${_executable_rpath}"
    INSTALL_RPATH_USE_LINK_PATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
    BUILD_RPATH_USE_ORIGIN TRUE)
  install(TARGETS "${target}"
    RUNTIME DESTINATION . COMPONENT Runtime)

  set(_runtime_targets)
  foreach(_dependency IN ITEMS
      wx::base wx::core wx::aui CURL::libcurl libssh2::libssh2)
    if(NOT TARGET "${_dependency}")
      message(FATAL_ERROR "Required runtime dependency is missing: ${_dependency}")
    endif()
    get_target_property(_concrete_target "${_dependency}" ALIASED_TARGET)
    if(NOT _concrete_target)
      set(_concrete_target "${_dependency}")
    endif()
    get_target_property(_type "${_concrete_target}" TYPE)
    get_target_property(_imported "${_concrete_target}" IMPORTED)
    if(_imported OR NOT _type STREQUAL "SHARED_LIBRARY")
      message(FATAL_ERROR
        "Runtime dependency ${_dependency} must be a project-built shared library")
    endif()
    list(APPEND _runtime_targets "${_concrete_target}")
  endforeach()
  list(REMOVE_DUPLICATES _runtime_targets)
  foreach(_library IN LISTS _runtime_targets)
    set_target_properties("${_library}" PROPERTIES
      INSTALL_RPATH "${_library_rpath}"
      INSTALL_RPATH_USE_LINK_PATH FALSE
      BUILD_WITH_INSTALL_RPATH FALSE
      BUILD_RPATH_USE_ORIGIN TRUE)
    if(APPLE)
      set_target_properties("${_library}" PROPERTIES
        MACOSX_RPATH TRUE INSTALL_NAME_DIR "@rpath")
    endif()
    # TARGETS installs the versioned file and SONAME link together. Development
    # namelinks are unnecessary in the runtime archive.
    install(TARGETS "${_library}"
      LIBRARY DESTINATION lib COMPONENT Runtime NAMELINK_SKIP)
  endforeach()

  install(FILES ${HAVREMOTE_TRANSLATION_FILES}
    DESTINATION translations COMPONENT Runtime)
  install(DIRECTORY "$<TARGET_FILE_DIR:${target}>/help/"
    DESTINATION help COMPONENT Runtime)
  install(FILES
    "${PROJECT_SOURCE_DIR}/resources/icons/havRemote.png"
    "${PROJECT_SOURCE_DIR}/resources/icons/havRemote.ico"
    DESTINATION icons COMPONENT Runtime)
  install(FILES
    "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION . RENAME LICENSE.txt COMPONENT Runtime)
  install(FILES
    "${PROJECT_SOURCE_DIR}/thirdPartyNotices.md"
    DESTINATION . COMPONENT Runtime)
  install(FILES "${wxwidgets_SOURCE_DIR}/docs/licence.txt"
    DESTINATION licenses RENAME wxWidgets-LICENSE.txt COMPONENT Runtime)
  install(FILES "${wxwidgets_SOURCE_DIR}/docs/lgpl.txt"
    DESTINATION licenses RENAME wxWidgets-LGPL.txt COMPONENT Runtime)
  install(FILES "${wxwidgets_SOURCE_DIR}/src/zlib/LICENSE"
    DESTINATION licenses RENAME zlib-LICENSE.txt COMPONENT Runtime)
  install(FILES "${wxwidgets_SOURCE_DIR}/src/png/LICENSE"
    DESTINATION licenses RENAME libpng-LICENSE.txt COMPONENT Runtime)
  install(FILES "${wxwidgets_SOURCE_DIR}/3rdparty/nanosvg/LICENSE.txt"
    DESTINATION licenses RENAME NanoSVG-LICENSE.txt COMPONENT Runtime)
  install(FILES "${curl_SOURCE_DIR}/COPYING"
    DESTINATION licenses RENAME curl-LICENSE.txt COMPONENT Runtime)
  install(FILES "${libssh2_SOURCE_DIR}/COPYING"
    DESTINATION licenses RENAME libssh2-LICENSE.txt COMPONENT Runtime)
  install(FILES "${havcson_SOURCE_DIR}/LICENSE"
    DESTINATION licenses RENAME havCSON-LICENSE.txt COMPONENT Runtime)
  install(FILES "${PROJECT_SOURCE_DIR}/resources/font/OFL.txt"
    DESTINATION licenses RENAME Oswald-OFL-1.1.txt COMPONENT Runtime)

  set(_architecture "${CMAKE_SYSTEM_PROCESSOR}")
  if(APPLE AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
    set(_architecture "${CMAKE_OSX_ARCHITECTURES}")
  endif()
  string(TOLOWER "${_architecture}" _architecture)
  if(_architecture MATCHES "^(x86_64|amd64)$")
    set(_architecture x86_64)
  elseif(_architecture MATCHES "^(aarch64|arm64)$")
    set(_architecture arm64)
  else()
    message(STATUS
      "Runtime installation is available. Release archives are not named for architecture '${_architecture}'")
    return()
  endif()

  if(APPLE)
    set(_platform macos)
    set(CPACK_GENERATOR ZIP)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_platform linux)
    set(CPACK_GENERATOR TGZ)
  else()
    return()
  endif()
  set(CPACK_PACKAGE_NAME havRemote)
  set(CPACK_PACKAGE_VENDOR "René Nicolaus")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME
    "havRemote-${PROJECT_VERSION}-${_platform}-${_architecture}")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
  set(CPACK_STRIP_FILES OFF)
  set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${CMAKE_PROJECT_NAME};Runtime;/")
  include(CPack)
endfunction()
