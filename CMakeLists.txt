# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 4.0)

project(havRemote
  VERSION 0.1.0
  DESCRIPTION "A native client for remote file access and transfer"
  LANGUAGES C CXX
)

if(NOT PROJECT_VERSION MATCHES
    "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
  message(FATAL_ERROR
    "havRemote releases require a canonical MAJOR.MINOR.PATCH project version")
endif()
foreach(_version_component IN ITEMS
    PROJECT_VERSION_MAJOR PROJECT_VERSION_MINOR PROJECT_VERSION_PATCH)
  if(${_version_component} GREATER 4294967295)
    message(FATAL_ERROR
      "${_version_component} must fit in an unsigned 32-bit integer")
  endif()
endforeach()
unset(_version_component)

if(WIN32)
  enable_language(RC)
endif()

option(HAVREMOTE_BUILD_TESTS "Build havRemote headless tests" ON)
option(HAVREMOTE_BUILD_APP "Build the wxWidgets desktop application" ON)
option(HAVREMOTE_BUILD_INTEGRATION_TESTS "Build container-backed integration tests" OFF)
option(HAVREMOTE_CONFIGURE_ONLY
  "Validate the CMake target graph without fetching dependencies (not buildable)" OFF)
mark_as_advanced(HAVREMOTE_CONFIGURE_ONLY)

set(HAVREMOTE_UPDATE_REPOSITORY_OWNER "Havoc7891" CACHE STRING
  "GitHub owner used by havRemote's public release checker")
set(HAVREMOTE_UPDATE_REPOSITORY_NAME "havRemote" CACHE STRING
  "GitHub repository used by havRemote's public release checker")
foreach(_update_repository_part IN ITEMS
    HAVREMOTE_UPDATE_REPOSITORY_OWNER HAVREMOTE_UPDATE_REPOSITORY_NAME)
  string(LENGTH "${${_update_repository_part}}" _update_repository_part_length)
  if(_update_repository_part_length GREATER 100 OR
     NOT "${${_update_repository_part}}" MATCHES
         "^[A-Za-z0-9][A-Za-z0-9_.-]*$")
    message(FATAL_ERROR
      "${_update_repository_part} must be a safe GitHub owner/repository name")
  endif()
endforeach()
unset(_update_repository_part)
unset(_update_repository_part_length)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT HAVREMOTE_CONFIGURE_ONLY)
  include(cmake/HavRemoteToolchainChecks.cmake)
endif()

include(cmake/HavRemoteProjectOptions.cmake)
if(HAVREMOTE_GNU_MINGW AND NOT HAVREMOTE_CONFIGURE_ONLY)
  find_package(Threads REQUIRED)
  havremote_enforce_static_mingw_threads()
endif()
if(NOT HAVREMOTE_CONFIGURE_ONLY)
  havremote_verify_cxx_capabilities()
endif()
include(cmake/HavRemoteDependencies.cmake)
include(cmake/HavRemotePackaging.cmake)

if(HAVREMOTE_GNU_MINGW AND NOT HAVREMOTE_CONFIGURE_ONLY)
  foreach(_runtime_target IN LISTS HAVREMOTE_RUNTIME_DLL_TARGETS)
    havremote_apply_mingw_static_runtime("${_runtime_target}")
  endforeach()
  unset(_runtime_target)
endif()

set(HAVREMOTE_PUBLIC_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")

if(HAVREMOTE_CONFIGURE_ONLY)
  set(_havremote_configure_only_source
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/havremote-configure-only.cpp")
  file(WRITE "${_havremote_configure_only_source}"
    "// Generated only for CMake graph validation.\n")
endif()

function(havremote_component_sources output)
  if(HAVREMOTE_CONFIGURE_ONLY)
    set("${output}" "${_havremote_configure_only_source}" PARENT_SCOPE)
  else()
    set("${output}" ${ARGN} PARENT_SCOPE)
  endif()
endfunction()

havremote_component_sources(_havremote_core_sources
  src/core/logSanitizer.cpp
  src/core/transferQueue.cpp
  src/core/transferRuntime.cpp
  src/core/types.cpp
)
add_library(havremote_core STATIC ${_havremote_core_sources})
target_include_directories(havremote_core PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
if(WIN32)
  target_link_libraries(havremote_core PRIVATE HavRemoteLibreSSL::Crypto)
else()
  if(NOT HAVREMOTE_CONFIGURE_ONLY)
    find_package(OpenSSL REQUIRED)
  endif()
  target_link_libraries(havremote_core PRIVATE OpenSSL::Crypto)
endif()
havremote_apply_defaults(havremote_core)

havremote_component_sources(_havremote_localization_sources
  src/localization/translationCatalog.cpp
)
add_library(havremote_localization STATIC ${_havremote_localization_sources})
target_include_directories(havremote_localization PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
target_link_libraries(havremote_localization PRIVATE havCSON::havCSON)
havremote_apply_defaults(havremote_localization)

file(GLOB HAVREMOTE_TRANSLATION_FILES CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/translations/*.cson"
)
if(NOT HAVREMOTE_TRANSLATION_FILES)
  message(FATAL_ERROR "havRemote requires at least one translations/*.cson catalog")
endif()

set(HAVREMOTE_HELP_SOURCE_DIR "${PROJECT_SOURCE_DIR}/resources/help")
file(GLOB_RECURSE HAVREMOTE_HELP_FILES CONFIGURE_DEPENDS
  "${HAVREMOTE_HELP_SOURCE_DIR}/*"
)
if(NOT EXISTS "${HAVREMOTE_HELP_SOURCE_DIR}/en/index.html")
  message(FATAL_ERROR "havRemote requires resources/help/en/index.html")
endif()
if(NOT EXISTS "${HAVREMOTE_HELP_SOURCE_DIR}/LICENSE.txt")
  message(FATAL_ERROR "havRemote's offline help requires resources/help/LICENSE.txt")
endif()
file(SHA256 "${PROJECT_SOURCE_DIR}/LICENSE" _havremote_license_hash)
file(SHA256 "${HAVREMOTE_HELP_SOURCE_DIR}/LICENSE.txt" _havremote_help_license_hash)
if(NOT _havremote_help_license_hash STREQUAL _havremote_license_hash)
  message(FATAL_ERROR
    "resources/help/LICENSE.txt must be identical to the repository LICENSE")
endif()
unset(_havremote_help_license_hash)
unset(_havremote_license_hash)
set(HAVREMOTE_HELP_LOGO
  "${PROJECT_SOURCE_DIR}/resources/icons/havRemote.png")
if(NOT EXISTS "${HAVREMOTE_HELP_LOGO}")
  message(FATAL_ERROR "havRemote's high-resolution help logo is missing")
endif()

havremote_component_sources(_havremote_config_sources
  src/config/configPathProvider.cpp
  src/config/csonConfigRepository.cpp
  src/config/csonQueueRepository.cpp
  src/config/csonDocumentEditor.cpp
  src/config/siteTransfer.cpp
)
add_library(havremote_config STATIC ${_havremote_config_sources})
target_include_directories(havremote_config PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
target_link_libraries(havremote_config
  PUBLIC
    havremote_core
    havCSON::havCSON
)
havremote_apply_defaults(havremote_config)

havremote_component_sources(_havremote_curl_runtime_sources
  src/network/curlRuntime.cpp
)
add_library(havremote_curl_runtime STATIC ${_havremote_curl_runtime_sources})
target_include_directories(havremote_curl_runtime
  PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(havremote_curl_runtime PRIVATE CURL::libcurl)
havremote_apply_defaults(havremote_curl_runtime)

havremote_component_sources(_havremote_update_sources
  src/update/updateService.cpp
)
add_library(havremote_update STATIC ${_havremote_update_sources})
target_include_directories(havremote_update
  PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
target_link_libraries(havremote_update
  PRIVATE
    havremote_curl_runtime
    CURL::libcurl
    havCSON::havCSON
)
havremote_apply_defaults(havremote_update)

havremote_component_sources(_havremote_ftp_sources
  src/protocol/ftpErrors.cpp
  src/protocol/ftpSession.cpp
)
add_library(havremote_ftp STATIC ${_havremote_ftp_sources})
target_include_directories(havremote_ftp PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
target_link_libraries(havremote_ftp
  PUBLIC havremote_core
  PRIVATE
    havremote_curl_runtime
    CURL::libcurl
)
if(WIN32)
  target_link_libraries(havremote_ftp PRIVATE crypt32 iphlpapi ws2_32)
elseif(NOT HAVREMOTE_CONFIGURE_ONLY)
  target_link_libraries(havremote_ftp PRIVATE wx::base OpenSSL::SSL OpenSSL::Crypto)
endif()
havremote_apply_defaults(havremote_ftp)

havremote_component_sources(_havremote_sftp_sources
  src/protocol/sftpErrors.cpp
  src/protocol/sftpHostKeyPolicy.cpp
  src/protocol/sftpKeyFiles.cpp
  src/protocol/sftpSession.cpp
)
add_library(havremote_sftp STATIC ${_havremote_sftp_sources})
target_include_directories(havremote_sftp PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
target_link_libraries(havremote_sftp
  PUBLIC havremote_core
  PRIVATE libssh2::libssh2
)
if(WIN32)
  target_link_libraries(havremote_sftp PRIVATE
    user32
    ws2_32
  )
endif()
havremote_apply_defaults(havremote_sftp)

havremote_component_sources(_havremote_platform_sources
  src/platform/externalEditor.cpp
  src/platform/reportExport.cpp
  src/platform/helpLauncher.cpp
  src/platform/webBrowser.cpp
  src/platform/credentialStore.cpp
  src/platform/credentialCleanup.cpp
  src/platform/pathSafety.cpp
  src/platform/destinationNames.cpp
)
if(TARGET wx::core AND NOT HAVREMOTE_CONFIGURE_ONLY)
  list(APPEND _havremote_platform_sources
    src/platform/recycleBin.cpp
    src/platform/systemTrash.cpp
  )
endif()
add_library(havremote_platform STATIC ${_havremote_platform_sources})
target_include_directories(havremote_platform PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
target_link_libraries(havremote_platform
  PUBLIC havremote_core
  PRIVATE wx::base
)
if(TARGET wx::core)
  target_link_libraries(havremote_platform PRIVATE wx::core)
endif()
if(WIN32)
  target_link_libraries(havremote_platform PRIVATE
    advapi32
    ole32
    shell32
  )
endif()
havremote_apply_defaults(havremote_platform)

if(HAVREMOTE_BUILD_APP AND NOT HAVREMOTE_CONFIGURE_ONLY AND
   (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug"))
  # Update simulation is available only in Debug builds.
  add_library(havremote_update_simulation STATIC
    src/update/updateSimulation.cpp
    src/update/updateSimulationWorkspace.cpp
  )
  target_compile_definitions(havremote_update_simulation
    PUBLIC "$<$<CONFIG:Debug>:HAVREMOTE_UPDATE_SIMULATION>")
  target_link_libraries(havremote_update_simulation
    PUBLIC havremote_update havremote_platform)
  havremote_apply_defaults(havremote_update_simulation)
endif()

havremote_component_sources(_havremote_file_list_model_sources
  src/ui/connectionProfileModel.cpp
  src/ui/fileListModel.cpp
  src/ui/messageLogModel.cpp
  src/ui/siteManagerModel.cpp
  src/ui/transferListModel.cpp
)
add_library(havremote_file_list_model STATIC
  ${_havremote_file_list_model_sources})
target_include_directories(havremote_file_list_model
  PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
target_link_libraries(havremote_file_list_model PUBLIC havremote_core)
havremote_apply_defaults(havremote_file_list_model)

if(HAVREMOTE_BUILD_APP)
havremote_component_sources(_havremote_ui_sources
  src/ui/aboutDialog.cpp
  src/ui/fileListCtrl.cpp
  src/ui/localDirectoryLoader.cpp
  src/ui/mainFrame.cpp
  src/ui/remotePermissionsDialog.cpp
  src/ui/remoteController.cpp
  src/ui/siteManagerDialog.cpp
  src/ui/splitButton.cpp
  src/ui/updateDialog.cpp
)
add_library(havremote_ui STATIC ${_havremote_ui_sources})
set(_havremote_generated_include_dir
  "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${_havremote_generated_include_dir}")
include(cmake/HavRemoteImages.cmake)
havremote_generate_images("${_havremote_generated_include_dir}/havRemoteImages.hpp")
configure_file(
  cmake/HavRemoteVersion.hpp.in
  "${_havremote_generated_include_dir}/havRemoteVersion.hpp"
  @ONLY
)
target_include_directories(havremote_ui
  PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}"
         "${_havremote_generated_include_dir}"
)
  target_link_libraries(havremote_ui
    PUBLIC
      havremote_core
      havremote_config
      havremote_localization
      havremote_platform
      havremote_file_list_model
      wx::base
      wx::core
      wx::aui
    PRIVATE
      havremote_ftp
      havremote_sftp
      havremote_update
  )
  if(WIN32)
    target_link_libraries(havremote_ui PRIVATE iphlpapi ws2_32)
  endif()
havremote_apply_defaults(havremote_ui)
if(TARGET havremote_update_simulation)
  target_link_libraries(havremote_ui PUBLIC havremote_update_simulation)
endif()

set(_havremote_icon "${PROJECT_SOURCE_DIR}/resources/icons/havRemote.ico")
if(WIN32)
  set(_havremote_manifest_template
    "${PROJECT_SOURCE_DIR}/resources/havRemote.manifest.in")
  set(_havremote_manifest
    "${CMAKE_CURRENT_BINARY_DIR}/havRemote.manifest")
  set(_havremote_png_resources
    "${PROJECT_SOURCE_DIR}/resources/clear.png"
    "${PROJECT_SOURCE_DIR}/resources/clearhistory.png"
    "${PROJECT_SOURCE_DIR}/resources/connect.png"
    "${PROJECT_SOURCE_DIR}/resources/disconnect.png"
    "${PROJECT_SOURCE_DIR}/resources/download.png"
    "${PROJECT_SOURCE_DIR}/resources/file.png"
    "${PROJECT_SOURCE_DIR}/resources/folder.png"
    "${PROJECT_SOURCE_DIR}/resources/history.png"
    "${PROJECT_SOURCE_DIR}/resources/plus.png"
    "${PROJECT_SOURCE_DIR}/resources/refresh.png"
    "${PROJECT_SOURCE_DIR}/resources/sitemanager.png"
    "${PROJECT_SOURCE_DIR}/resources/symlink.png"
    "${PROJECT_SOURCE_DIR}/resources/unknown.png"
    "${PROJECT_SOURCE_DIR}/resources/up.png"
    "${PROJECT_SOURCE_DIR}/resources/upload.png"
  )
  set(_havremote_manifest_version_tweak "${PROJECT_VERSION_TWEAK}")
  if(_havremote_manifest_version_tweak STREQUAL "")
    set(_havremote_manifest_version_tweak 0)
  endif()
  set(HAVREMOTE_MANIFEST_VERSION
    "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${_havremote_manifest_version_tweak}")
  configure_file(
    "${_havremote_manifest_template}"
    "${_havremote_manifest}"
    @ONLY
  )
  set(_havremote_icon
    "${PROJECT_SOURCE_DIR}/resources/icons/havRemote.ico")
  cmake_path(CONVERT "${PROJECT_SOURCE_DIR}/resources"
    TO_CMAKE_PATH_LIST HAVREMOTE_RESOURCE_PATH NORMALIZE)
  cmake_path(CONVERT "${_havremote_manifest}"
    TO_CMAKE_PATH_LIST HAVREMOTE_MANIFEST_PATH NORMALIZE)
  cmake_path(CONVERT "${_havremote_icon}"
    TO_CMAKE_PATH_LIST HAVREMOTE_ICON_PATH NORMALIZE)
  configure_file(resources/havRemote.rc.in "${CMAKE_CURRENT_BINARY_DIR}/havRemote.rc" @ONLY)
  set(_havremote_resource "${CMAKE_CURRENT_BINARY_DIR}/havRemote.rc")
  set_property(SOURCE "${_havremote_resource}" APPEND PROPERTY
    OBJECT_DEPENDS
      "${_havremote_manifest}"
      "${_havremote_icon}"
      ${_havremote_png_resources}
  )
  unset(HAVREMOTE_MANIFEST_VERSION)
  unset(HAVREMOTE_RESOURCE_PATH)
  unset(_havremote_png_resources)
  unset(_havremote_manifest_version_tweak)
  unset(_havremote_manifest_template)
endif()

havremote_component_sources(_havremote_application_sources
  src/main.cpp
  ${_havremote_resource}
)
add_executable(havRemote WIN32 ${_havremote_application_sources})
target_link_libraries(havRemote PRIVATE havremote_ui)
havremote_apply_defaults(havRemote)
havremote_apply_mingw_static_runtime(havRemote)
set_target_properties(havRemote PROPERTIES
  OUTPUT_NAME havRemote
  DEBUG_POSTFIX "-debug"
)
if(MSVC)
  # The resource script already embeds the complete application manifest.
  # Disable MSVC's automatically generated manifest to avoid a second ID 1.
  target_link_options(havRemote PRIVATE /MANIFEST:NO)
endif()
havremote_enable_windows_packaging(havRemote)

if(NOT HAVREMOTE_CONFIGURE_ONLY)
  add_custom_target(havremote_translation_assets ALL
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/translations"
      "-DDESTINATION_DIR=$<TARGET_FILE_DIR:havRemote>/translations"
      "-DALLOWED_ROOT=${CMAKE_BINARY_DIR}"
      -P "${PROJECT_SOURCE_DIR}/cmake/scripts/SyncTranslations.cmake"
    DEPENDS
      ${HAVREMOTE_TRANSLATION_FILES}
      "${PROJECT_SOURCE_DIR}/cmake/scripts/SyncTranslations.cmake"
    VERBATIM
  )
  add_dependencies(havRemote havremote_translation_assets)
  set_target_properties(havremote_translation_assets PROPERTIES
    FOLDER "Resources")

  add_custom_target(havremote_help_assets ALL
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${HAVREMOTE_HELP_SOURCE_DIR}"
      "-DDESTINATION_DIR=$<TARGET_FILE_DIR:havRemote>/help"
      "-DALLOWED_ROOT=${CMAKE_BINARY_DIR}"
      "-DHAVREMOTE_HELP_VERSION=${PROJECT_VERSION}"
      -P "${PROJECT_SOURCE_DIR}/cmake/scripts/SyncHelp.cmake"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "$<TARGET_FILE_DIR:havRemote>/icons"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${HAVREMOTE_HELP_LOGO}"
      "$<TARGET_FILE_DIR:havRemote>/icons/havRemote.png"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${_havremote_icon}"
      "$<TARGET_FILE_DIR:havRemote>/icons/havRemote.ico"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${PROJECT_SOURCE_DIR}/LICENSE"
      "$<TARGET_FILE_DIR:havRemote>/LICENSE.txt"
    DEPENDS
      ${HAVREMOTE_HELP_FILES}
      "${HAVREMOTE_HELP_LOGO}"
      "${_havremote_icon}"
      "${PROJECT_SOURCE_DIR}/LICENSE"
      "${PROJECT_SOURCE_DIR}/cmake/scripts/SyncHelp.cmake"
    VERBATIM
  )
  add_dependencies(havRemote havremote_help_assets)
  set_target_properties(havremote_help_assets PROPERTIES
    FOLDER "Resources")
endif()
include(cmake/HavRemoteUnixPackaging.cmake)
havremote_enable_unix_packaging(havRemote)
endif()

if(HAVREMOTE_BUILD_TESTS)
  enable_testing()
  include(cmake/HavRemoteTests.cmake)
endif()

if(WIN32 AND TARGET havRemote)
  install(TARGETS havRemote
    RUNTIME DESTINATION .
    COMPONENT Runtime
  )
  foreach(_runtime_target IN LISTS HAVREMOTE_RUNTIME_DLL_TARGETS)
    install(FILES "$<TARGET_FILE:${_runtime_target}>"
      DESTINATION .
      COMPONENT Runtime
    )
  endforeach()
  unset(_runtime_target)
  install(FILES ${HAVREMOTE_TRANSLATION_FILES}
    DESTINATION translations
    COMPONENT Runtime
  )
  install(DIRECTORY "$<TARGET_FILE_DIR:havRemote>/help/"
    DESTINATION help
    COMPONENT Runtime
  )
  install(FILES "${HAVREMOTE_HELP_LOGO}" "${_havremote_icon}"
    DESTINATION icons
    COMPONENT Runtime
  )
  install(FILES
    LICENSE
    DESTINATION . RENAME LICENSE.txt
    COMPONENT Runtime
  )
  install(FILES
    thirdPartyNotices.md
    DESTINATION .
    COMPONENT Runtime
  )
  if(NOT HAVREMOTE_CONFIGURE_ONLY)
    set(_havremote_third_party_provenance
      "${CMAKE_CURRENT_BINARY_DIR}/thirdPartyProvenance.txt")
    havremote_configure_third_party_provenance(
      "${_havremote_third_party_provenance}")
    install(FILES "${_havremote_third_party_provenance}"
      DESTINATION .
      COMPONENT Runtime
    )
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
    install(FILES "${libressl_SOURCE_DIR}/COPYING"
      DESTINATION licenses RENAME LibreSSL-LICENSE.txt COMPONENT Runtime)
    install(FILES "${havcson_SOURCE_DIR}/LICENSE"
      DESTINATION licenses RENAME havCSON-LICENSE.txt COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/resources/font/OFL.txt"
      DESTINATION licenses RENAME Oswald-OFL-1.1.txt COMPONENT Runtime)
    if(DEFINED catch2_SOURCE_DIR AND EXISTS "${catch2_SOURCE_DIR}/LICENSE.txt")
      install(FILES "${catch2_SOURCE_DIR}/LICENSE.txt"
        DESTINATION licenses RENAME Catch2-LICENSE.txt COMPONENT Runtime)
    endif()
    if(HAVREMOTE_GNU_MINGW AND DEFINED HAVREMOTE_MINGW_ROOT AND
       NOT HAVREMOTE_MINGW_ROOT STREQUAL "")
      havremote_resolve_mingw_licenses()
      install(FILES "${HAVREMOTE_GCC_GPLV3_LICENSE}"
        DESTINATION licenses RENAME GCC-GPLv3.txt COMPONENT Runtime)
      install(FILES "${HAVREMOTE_GCC_RUNTIME_EXCEPTION}"
        DESTINATION licenses RENAME GCC-Runtime-Library-Exception.txt COMPONENT Runtime)
      install(FILES "${HAVREMOTE_MINGW_RUNTIME_LICENSE}"
        DESTINATION licenses RENAME MinGW-w64-Runtime-LICENSE.txt COMPONENT Runtime)
      install(FILES "${HAVREMOTE_WINPTHREADS_LICENSE}"
        DESTINATION licenses RENAME winpthreads-LICENSE.txt COMPONENT Runtime)
      unset(HAVREMOTE_GCC_GPLV3_LICENSE)
      unset(HAVREMOTE_GCC_RUNTIME_EXCEPTION)
      unset(HAVREMOTE_MINGW_RUNTIME_LICENSE)
      unset(HAVREMOTE_WINPTHREADS_LICENSE)
    endif()
    unset(_havremote_third_party_provenance)
  endif()

  # Portable ZIP packaging and GNU symbol auditing require the MinGW toolchain.
  if(HAVREMOTE_AUDITED_PACKAGING)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_NAME havRemote)
    set(CPACK_PACKAGE_VENDOR "René Nicolaus")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "havRemote-${PROJECT_VERSION}-windows-x86_64")
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_STRIP_FILES OFF)
    include(CPack)
  endif()
endif()
