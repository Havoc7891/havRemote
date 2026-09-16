# SPDX-License-Identifier: MIT

include_guard(GLOBAL)

if(HAVREMOTE_CONFIGURE_ONLY)
  message(STATUS "Configure-only mode: test executables are omitted")
  return()
endif()

if(NOT TARGET Catch2::Catch2WithMain)
  message(FATAL_ERROR "HAVREMOTE_BUILD_TESTS is ON, but Catch2 is unavailable")
endif()

if(CMAKE_CONFIGURATION_TYPES)
  # Discover tests for the selected configuration
  set(CMAKE_CATCH_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
endif()

include(Catch)

if(TARGET havRemote)
  add_test(NAME app.smoke COMMAND havRemote --smoke-test)
  set_tests_properties(app.smoke PROPERTIES LABELS "ui;resources" TIMEOUT 30)
endif()

add_test(NAME build.compilerCapabilities
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/compiler-check-tests"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
    "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DTEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    -DEXPECT_SUCCESS=ON
    -P "${PROJECT_SOURCE_DIR}/tests/cmake/toolchainChecksTests.cmake")
set_tests_properties(build.compilerCapabilities PROPERTIES
  LABELS "build;compiler"
  TIMEOUT 120)

add_test(NAME build.libresslShim
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/shim-tests"
    "-DC_COMPILER=${CMAKE_C_COMPILER}"
    "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
    "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DTEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    -P "${PROJECT_SOURCE_DIR}/tests/cmake/libresslShimTests.cmake")
set_tests_properties(build.libresslShim PROPERTIES
  LABELS "build;dependencies"
  TIMEOUT 180)

add_test(NAME build.licensePackaging
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DTEST_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/license-packaging-tests"
    "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
    "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DTEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    -P "${PROJECT_SOURCE_DIR}/tests/cmake/licensePackagingTests.cmake")
set_tests_properties(build.licensePackaging PROPERTIES
  LABELS "build;packaging;resources"
  TIMEOUT 120)

if(WIN32)
  find_program(_havremote_test_powershell NAMES powershell pwsh)
  get_filename_component(_havremote_make_directory "${CMAKE_MAKE_PROGRAM}" DIRECTORY)
  find_program(_havremote_test_mingw_make NAMES mingw32-make
    HINTS "${_havremote_make_directory}" "$ENV{HAVREMOTE_MINGW_ROOT}/bin")
  if(_havremote_test_powershell AND _havremote_test_mingw_make)
    add_test(NAME build.helperPresets
      COMMAND "${_havremote_test_powershell}"
        -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tests/buildHelper/buildHelperTests.ps1"
        -SourceDirectory "${PROJECT_SOURCE_DIR}"
        -TestBinaryRoot "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/build-helper-tests"
        -CMakePath "${CMAKE_COMMAND}"
        -MakeProgram "${_havremote_test_mingw_make}")
    set_tests_properties(build.helperPresets PROPERTIES
      LABELS "build;presets"
      TIMEOUT 180)
  endif()
endif()

# Test path validation with a fake native backend, never the user's Trash
add_executable(havremote_recycle_bin_tests
  tests/platform/recycleBinTests.cpp
  src/platform/recycleBin.cpp)
target_include_directories(havremote_recycle_bin_tests PRIVATE
  "${HAVREMOTE_PUBLIC_INCLUDE_DIR}" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(havremote_recycle_bin_tests PRIVATE Catch2::Catch2WithMain)
havremote_apply_defaults(havremote_recycle_bin_tests)
havremote_apply_mingw_static_runtime(havremote_recycle_bin_tests)
catch_discover_tests(havremote_recycle_bin_tests)

set(_havremote_regular_test_sources)
foreach(_candidate IN ITEMS
    tests/core/typesTests.cpp
    tests/core/fileTimeTests.cpp
    tests/core/logSanitizerTests.cpp
    tests/core/transferQueueTests.cpp
    tests/core/transferRuntimeTests.cpp
    tests/config/csonDocumentEditorTests.cpp
    tests/config/csonConfigRepositoryTests.cpp
    tests/config/csonQueueRepositoryTests.cpp
    tests/config/siteTransferTests.cpp
    tests/localization/translationCatalogTests.cpp
    tests/platform/helpLauncherTests.cpp
    tests/platform/credentialStoreTests.cpp
    tests/platform/credentialCleanupTests.cpp
    tests/platform/toolkitPathsTests.cpp
    tests/platform/webBrowserTests.cpp
    tests/platform/reportExportTests.cpp
    tests/update/updateServiceTests.cpp
    tests/protocol/ftpErrorsTests.cpp
    tests/protocol/ftpControlTraceTests.cpp
    tests/protocol/ftpUploadReplyTests.cpp
    tests/protocol/ftpListingTests.cpp
    tests/protocol/ftpSecurityTests.cpp
    tests/protocol/sftpErrorsTests.cpp
    tests/protocol/sftpKeyFilesTests.cpp
    tests/protocol/sftpMetadataTests.cpp
    tests/protocol/sftpSocketTests.cpp
    tests/ui/connectionProfileModelTests.cpp
    tests/ui/localDirectoryEventsTests.cpp
    tests/ui/localDirectoryWorkerLimiterTests.cpp
    tests/ui/fileListCtrlTests.cpp
    tests/ui/messageLogModelTests.cpp
    tests/ui/siteManagerModelTests.cpp
    tests/ui/transferListModelTests.cpp)
  if(EXISTS "${PROJECT_SOURCE_DIR}/${_candidate}")
    list(APPEND _havremote_regular_test_sources "${_candidate}")
  endif()
endforeach()

if(WIN32)
  list(APPEND _havremote_regular_test_sources tests/protocol/ftpSessionFailureTests.cpp)
endif()
if(TARGET wx::core)
  list(APPEND _havremote_regular_test_sources tests/platform/systemTrashTests.cpp)
endif()

list(APPEND _havremote_regular_test_sources tests/platform/pathSafetyTests.cpp)
if(EXISTS "${PROJECT_SOURCE_DIR}/tests/platform/externalEditorTests.cpp")
  list(APPEND _havremote_regular_test_sources tests/platform/externalEditorTests.cpp)
endif()
if(WIN32 AND TARGET wx::core AND EXISTS "${PROJECT_SOURCE_DIR}/tests/ui/buttonBitmapTests.cpp")
  list(APPEND _havremote_regular_test_sources tests/ui/buttonBitmapTests.cpp)
endif()
if(_havremote_regular_test_sources)
  add_executable(havremote_tests ${_havremote_regular_test_sources})
  target_include_directories(havremote_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  if(WIN32 AND TARGET wx::core)
    if(NOT DEFINED _havremote_manifest OR
       NOT EXISTS "${_havremote_manifest}")
      message(FATAL_ERROR
        "Windows UI-linked tests require havRemote's generated manifest resource")
    endif()
    # The manifest activates Common Controls v6, required by wx::core for
    # GetWindowSubclass.
    cmake_path(CONVERT "${_havremote_manifest}"
      TO_CMAKE_PATH_LIST HAVREMOTE_TEST_MANIFEST_PATH NORMALIZE)
    set(_havremote_test_resource
      "${CMAKE_CURRENT_BINARY_DIR}/havRemoteTests.rc")
    configure_file(
      "${PROJECT_SOURCE_DIR}/resources/havRemoteTests.rc.in"
      "${_havremote_test_resource}"
      @ONLY)
    set_property(SOURCE "${_havremote_test_resource}" APPEND PROPERTY
      OBJECT_DEPENDS "${_havremote_manifest}")
    target_sources(havremote_tests PRIVATE "${_havremote_test_resource}")
    if(MSVC)
      target_link_options(havremote_tests PRIVATE /MANIFEST:NO)
    endif()
    unset(HAVREMOTE_TEST_MANIFEST_PATH)
    unset(_havremote_test_resource)
  endif()
  target_link_libraries(havremote_tests PRIVATE
    Catch2::Catch2WithMain
    havremote_config
    havremote_localization
    havremote_ftp
    CURL::libcurl
    havremote_sftp
    libssh2::libssh2
    havremote_platform
    havremote_update
    havremote_file_list_model
    wx::base
  )
  if(TARGET wx::core)
    target_link_libraries(havremote_tests PRIVATE wx::core)
  endif()
  if(WIN32)
    target_link_libraries(havremote_tests PRIVATE iphlpapi ws2_32)
  endif()
  target_compile_definitions(havremote_tests PRIVATE
    HAVREMOTE_TRANSLATIONS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/translations"
  )
  havremote_apply_defaults(havremote_tests)
  havremote_apply_mingw_static_runtime(havremote_tests)
  if(TARGET havremote_update_simulation)
    target_sources(havremote_tests PRIVATE
      tests/update/updateSimulationTests.cpp
      tests/update/updateSimulationWorkspaceTests.cpp
    )
    target_link_libraries(havremote_tests PRIVATE havremote_update_simulation)
  endif()
  catch_discover_tests(havremote_tests)
endif()

if(HAVREMOTE_BUILD_APP)
  # Native button geometry requires a wxApp and a display. Register the whole
  # executable directly so Catch discovery does not launch GUI code at build
  # time. Linux UI runs can provide their display with Xvfb.
  add_executable(havremote_button_layout_tests
    tests/ui/buttonLayoutTests.cpp
    src/ui/splitButton.cpp
  )
  target_include_directories(havremote_button_layout_tests PRIVATE
    "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
  if(WIN32)
    target_sources(havremote_button_layout_tests PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/havRemoteTests.rc")
  endif()
  if(MSVC)
    target_link_options(havremote_button_layout_tests PRIVATE /MANIFEST:NO)
  endif()
  target_link_libraries(havremote_button_layout_tests PRIVATE
    Catch2::Catch2
    wx::base
    wx::core
    wx::aui
  )
  havremote_apply_defaults(havremote_button_layout_tests)
  havremote_apply_mingw_static_runtime(havremote_button_layout_tests)
  add_test(NAME ui.button-layout COMMAND havremote_button_layout_tests)
  set_tests_properties(ui.button-layout PROPERTIES LABELS ui TIMEOUT 30)

  # Compile the controller against test-owned protocol factories to check
  # worker/UI persistence barriers without a server.
  add_executable(havremote_controller_tests
    tests/ui/remoteControllerTests.cpp
    src/ui/remoteController.cpp
  )
  if(WIN32)
    target_sources(havremote_controller_tests PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/havRemoteTests.rc")
  endif()
  if(MSVC)
    target_link_options(havremote_controller_tests PRIVATE /MANIFEST:NO)
  endif()
  target_link_libraries(havremote_controller_tests PRIVATE
    Catch2::Catch2WithMain
    havremote_core
    havremote_config
    havremote_platform
    wx::base
    wx::core
  )
  havremote_apply_defaults(havremote_controller_tests)
  havremote_apply_mingw_static_runtime(havremote_controller_tests)
  catch_discover_tests(havremote_controller_tests)
endif()

# havCSON's test-hook macro changes inline header definitions. Compile the
# repository implementation and atomic-failure tests together in an isolated
# executable so those definitions never mix with the production config target.
set(_havremote_atomic_test_sources)
foreach(_candidate IN ITEMS
    tests/config/csonConfigRepositoryAtomicTests.cpp
    tests/config/csonQueueRepositoryAtomicTests.cpp
    tests/config/siteTransferAtomicTests.cpp)
  if(EXISTS "${PROJECT_SOURCE_DIR}/${_candidate}")
    list(APPEND _havremote_atomic_test_sources "${_candidate}")
  endif()
endforeach()
if(_havremote_atomic_test_sources)
  add_library(havremote_config_test_hooks STATIC
    src/config/configPathProvider.cpp
    src/config/csonConfigRepository.cpp
    src/config/csonQueueRepository.cpp
    src/config/csonDocumentEditor.cpp
    src/config/siteTransfer.cpp
  )
  target_include_directories(havremote_config_test_hooks PUBLIC "${HAVREMOTE_PUBLIC_INCLUDE_DIR}")
  target_compile_definitions(havremote_config_test_hooks PUBLIC HAVCSON_ENABLE_TEST_HOOKS)
  target_link_libraries(havremote_config_test_hooks PUBLIC
    havremote_core
    havCSON::havCSON
  )
  havremote_apply_defaults(havremote_config_test_hooks)

  add_executable(havremote_cson_atomic_tests ${_havremote_atomic_test_sources})
  target_compile_definitions(havremote_cson_atomic_tests PRIVATE HAVCSON_ENABLE_TEST_HOOKS)
  target_link_libraries(havremote_cson_atomic_tests PRIVATE
    Catch2::Catch2WithMain
    havremote_config_test_hooks
  )
  havremote_apply_defaults(havremote_cson_atomic_tests)
  havremote_apply_mingw_static_runtime(havremote_cson_atomic_tests)
  catch_discover_tests(havremote_cson_atomic_tests)
endif()

if(HAVREMOTE_BUILD_INTEGRATION_TESTS)
  add_executable(havremote_integration_tests
    tests/integration/protocolIntegrationTests.cpp
    tests/integration/ftpIdleLifecycleTests.cpp
    tests/integration/sftpReadAheadTests.cpp
  )
  target_link_libraries(havremote_integration_tests PRIVATE
    Catch2::Catch2WithMain
    havremote_core
    havremote_ftp
    havremote_sftp
  )
  if(WIN32)
    target_link_libraries(havremote_integration_tests PRIVATE
      havremote_config havremote_platform)
  endif()
  havremote_apply_defaults(havremote_integration_tests)
  havremote_apply_mingw_static_runtime(havremote_integration_tests)

  add_test(NAME integration.ftp-idle
    COMMAND $<TARGET_FILE:havremote_integration_tests>
      "[ftp-idle]" --reporter compact --durations yes
  )
  set_tests_properties(integration.ftp-idle PROPERTIES
    LABELS "integration;manual;ftp-idle"
    SKIP_REGULAR_EXPRESSION "Set HAVREMOTE_FTP_IDLE_HOST"
    TIMEOUT 180
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  )

  add_test(NAME integration.active-ftps
    COMMAND $<TARGET_FILE:havremote_integration_tests>
      "[active-ftps]" --reporter compact --durations yes
  )
  set_tests_properties(integration.active-ftps PROPERTIES
    LABELS "integration;manual;active-ftps"
    SKIP_REGULAR_EXPRESSION "Set HAVREMOTE_ACTIVE_FTPS_HOST"
    TIMEOUT 180
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  )

  find_program(HAVREMOTE_DOCKER_EXECUTABLE NAMES docker)
  if(NOT HAVREMOTE_DOCKER_EXECUTABLE)
    message(STATUS
      "Docker was not found. The reachable-server active-FTPS integration test remains available")
    return()
  endif()

  set(_havremote_integration_root "${PROJECT_SOURCE_DIR}/tests/integration")
  set(_havremote_compose_file "${_havremote_integration_root}/docker-compose.yml")
  if(NOT EXISTS "${_havremote_compose_file}")
    message(FATAL_ERROR
      "HAVREMOTE_BUILD_INTEGRATION_TESTS requires ${_havremote_compose_file}")
  endif()

  string(SHA1 _havremote_integration_hash "${CMAKE_BINARY_DIR}")
  string(SUBSTRING "${_havremote_integration_hash}" 0 8 _havremote_integration_hash)
  set(_havremote_compose_project "havremote-${_havremote_integration_hash}")
  set(_havremote_docker_fixture havremote_protocol_servers)
  set(_havremote_docker_lock havremote_integration_ports)

  add_test(NAME integration.docker.setup
    COMMAND "${HAVREMOTE_DOCKER_EXECUTABLE}" compose
      --project-name "${_havremote_compose_project}"
      --file "${_havremote_compose_file}"
      up --build --detach --wait --wait-timeout 180
  )
  set_tests_properties(integration.docker.setup PROPERTIES
    FIXTURES_SETUP "${_havremote_docker_fixture}"
    LABELS "integration;docker;fixture"
    RESOURCE_LOCK "${_havremote_docker_lock}"
    TIMEOUT 600
    WORKING_DIRECTORY "${_havremote_integration_root}"
  )

  add_test(NAME integration.docker.cleanup
    COMMAND "${HAVREMOTE_DOCKER_EXECUTABLE}" compose
      --project-name "${_havremote_compose_project}"
      --file "${_havremote_compose_file}"
      down --volumes --remove-orphans
  )
  set_tests_properties(integration.docker.cleanup PROPERTIES
    FIXTURES_CLEANUP "${_havremote_docker_fixture}"
    LABELS "integration;docker;fixture"
    RESOURCE_LOCK "${_havremote_docker_lock}"
    TIMEOUT 120
    WORKING_DIRECTORY "${_havremote_integration_root}"
  )

  set(_havremote_integration_environment
    "HAVREMOTE_INTEGRATION_HOST=127.0.0.1"
    "HAVREMOTE_FTP_PORT=2121"
    "HAVREMOTE_FTPS_PORT=2122"
    "HAVREMOTE_SFTP_PORT=2222"
    "HAVREMOTE_FTPS_PIN=sha256//L4IHQpsCHemCVp2BuMv+FQSwULTsaGuvyedqYug1Bf4="
  )
  foreach(_protocol IN ITEMS ftp ftps sftp)
    add_test(NAME "integration.${_protocol}"
      COMMAND $<TARGET_FILE:havremote_integration_tests>
        "[${_protocol}]" --reporter compact --durations yes
    )
    set_tests_properties("integration.${_protocol}" PROPERTIES
      ENVIRONMENT "${_havremote_integration_environment}"
      FIXTURES_REQUIRED "${_havremote_docker_fixture}"
      LABELS "integration;docker;${_protocol}"
      RESOURCE_LOCK "${_havremote_docker_lock}"
      TIMEOUT 180
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
  endforeach()

  message(STATUS
    "Docker-backed FTP/FTPS/SFTP tests enabled. See tests/integration/README.md")
endif()
