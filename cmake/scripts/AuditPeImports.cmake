# SPDX-License-Identifier: MIT

if(NOT DEFINED OBJDUMP OR NOT EXISTS "${OBJDUMP}")
  message(FATAL_ERROR "OBJDUMP must name the MinGW objdump executable")
endif()
if(NOT DEFINED PE_FILE OR NOT EXISTS "${PE_FILE}")
  message(FATAL_ERROR "PE_FILE does not exist: ${PE_FILE}")
endif()
if(NOT DEFINED RUNTIME_DLL_MANIFEST OR
   NOT EXISTS "${RUNTIME_DLL_MANIFEST}")
  message(FATAL_ERROR
    "RUNTIME_DLL_MANIFEST does not exist: ${RUNTIME_DLL_MANIFEST}")
endif()
if(NOT DEFINED RUNTIME_PACKAGE_DIR OR
   NOT IS_DIRECTORY "${RUNTIME_PACKAGE_DIR}")
  message(FATAL_ERROR
    "RUNTIME_PACKAGE_DIR is not a directory: ${RUNTIME_PACKAGE_DIR}")
endif()
if(NOT DEFINED TRANSLATION_SOURCE_DIR OR
   NOT IS_DIRECTORY "${TRANSLATION_SOURCE_DIR}")
  message(FATAL_ERROR
    "TRANSLATION_SOURCE_DIR is not a directory: ${TRANSLATION_SOURCE_DIR}")
endif()
if(NOT DEFINED TRANSLATION_PACKAGE_DIR OR
   NOT IS_DIRECTORY "${TRANSLATION_PACKAGE_DIR}")
  message(FATAL_ERROR
    "Packaged translations directory is missing: ${TRANSLATION_PACKAGE_DIR}")
endif()
if(NOT DEFINED HELP_SOURCE_DIR OR NOT IS_DIRECTORY "${HELP_SOURCE_DIR}")
  message(FATAL_ERROR "HELP_SOURCE_DIR is not a directory: ${HELP_SOURCE_DIR}")
endif()
if(NOT DEFINED HELP_PACKAGE_DIR OR NOT IS_DIRECTORY "${HELP_PACKAGE_DIR}")
  message(FATAL_ERROR "Packaged help directory is missing: ${HELP_PACKAGE_DIR}")
endif()
if(NOT DEFINED HAVREMOTE_HELP_VERSION OR
   NOT HAVREMOTE_HELP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "HAVREMOTE_HELP_VERSION must be MAJOR.MINOR.PATCH")
endif()
if(NOT DEFINED HELP_LOGO_SOURCE OR NOT EXISTS "${HELP_LOGO_SOURCE}")
  message(FATAL_ERROR "HELP_LOGO_SOURCE does not exist: ${HELP_LOGO_SOURCE}")
endif()
if(NOT DEFINED HELP_LOGO_PACKAGE OR NOT EXISTS "${HELP_LOGO_PACKAGE}")
  message(FATAL_ERROR "Packaged help logo is missing: ${HELP_LOGO_PACKAGE}")
endif()
if(NOT DEFINED HELP_FAVICON_SOURCE OR NOT EXISTS "${HELP_FAVICON_SOURCE}")
  message(FATAL_ERROR "HELP_FAVICON_SOURCE does not exist: ${HELP_FAVICON_SOURCE}")
endif()
if(NOT DEFINED HELP_FAVICON_PACKAGE OR NOT EXISTS "${HELP_FAVICON_PACKAGE}")
  message(FATAL_ERROR "Packaged help favicon is missing: ${HELP_FAVICON_PACKAGE}")
endif()
if(NOT DEFINED LICENSE_SOURCE OR NOT EXISTS "${LICENSE_SOURCE}")
  message(FATAL_ERROR "LICENSE_SOURCE does not exist: ${LICENSE_SOURCE}")
endif()
if(NOT DEFINED LICENSE_PACKAGE OR NOT EXISTS "${LICENSE_PACKAGE}")
  message(FATAL_ERROR "Staged license is missing: ${LICENSE_PACKAGE}")
endif()

set(_local_build_path_prefixes)
if(ENFORCE_REPRODUCIBLE_PATHS)
  if(NOT DEFINED LOCAL_BUILD_PATH_MANIFEST OR
     NOT EXISTS "${LOCAL_BUILD_PATH_MANIFEST}")
    message(FATAL_ERROR
      "LOCAL_BUILD_PATH_MANIFEST is required for Release PE path auditing")
  endif()
  file(STRINGS "${LOCAL_BUILD_PATH_MANIFEST}" _local_build_path_lines
    ENCODING UTF-8)
  foreach(_local_build_path IN LISTS _local_build_path_lines)
    string(STRIP "${_local_build_path}" _local_build_path)
    if(_local_build_path STREQUAL "")
      continue()
    endif()
    string(REPLACE "\\" "/" _local_build_path "${_local_build_path}")
    string(REGEX REPLACE "/+$" "" _local_build_path "${_local_build_path}")
    string(TOLOWER "${_local_build_path}" _local_build_path)
    list(APPEND _local_build_path_prefixes "${_local_build_path}")
  endforeach()
  list(REMOVE_DUPLICATES _local_build_path_prefixes)
  if(NOT _local_build_path_prefixes)
    message(FATAL_ERROR "The Release PE path-audit manifest is empty")
  endif()
endif()

# Reject dynamic MinGW runtime imports, including thread-model and
# exception-runtime naming variations.
set(_forbidden_runtime_regex
  "^(LIBSTDC\\+\\+|LIBGCC|LIBWINPTHREAD|LIBATOMIC|LIBSSP|LIBGOMP|LIBQUADMATH|MSYS-|CYGWIN).*\\.DLL$")

# The generated manifest is the single source of truth for non-system runtime
# DLLs. It contains target-derived basenames, one per line.
file(STRINGS "${RUNTIME_DLL_MANIFEST}" _runtime_manifest_lines
  ENCODING UTF-8)
set(_expected_runtime_dll_names)
set(_expected_runtime_dll_names_upper)
foreach(_manifest_name IN LISTS _runtime_manifest_lines)
  string(STRIP "${_manifest_name}" _manifest_name)
  if(_manifest_name STREQUAL "")
    continue()
  endif()

  cmake_path(GET _manifest_name FILENAME _manifest_basename)
  if(NOT "${_manifest_name}" STREQUAL "${_manifest_basename}")
    message(FATAL_ERROR
      "Runtime DLL manifest entries must be basenames: ${_manifest_name}")
  endif()
  string(TOUPPER "${_manifest_name}" _manifest_name_upper)
  if(NOT _manifest_name_upper MATCHES "\\.DLL$")
    message(FATAL_ERROR
      "Runtime DLL manifest entry is not a DLL: ${_manifest_name}")
  endif()
  if(_manifest_name_upper MATCHES "${_forbidden_runtime_regex}")
    message(FATAL_ERROR
      "Runtime DLL manifest contains forbidden MinGW/MSYS runtime "
      "${_manifest_name}")
  endif()
  list(FIND _expected_runtime_dll_names_upper
    "${_manifest_name_upper}" _duplicate_index)
  if(NOT _duplicate_index EQUAL -1)
    message(FATAL_ERROR
      "Runtime DLL manifest contains a duplicate basename: ${_manifest_name}")
  endif()
  list(APPEND _expected_runtime_dll_names "${_manifest_name}")
  list(APPEND _expected_runtime_dll_names_upper "${_manifest_name_upper}")
endforeach()
if(NOT _expected_runtime_dll_names)
  message(FATAL_ERROR "Runtime DLL manifest is empty")
endif()

# Discover DLLs case-insensitively by examining all directory entries. A plain
# '*.dll' glob is case-sensitive on some hosts where this audit can be tested.
file(GLOB _runtime_package_entries LIST_DIRECTORIES FALSE
  "${RUNTIME_PACKAGE_DIR}/*")
set(_packaged_dll_names)
set(_packaged_dll_names_upper)
set(_packaged_dll_paths)
foreach(_package_entry IN LISTS _runtime_package_entries)
  cmake_path(GET _package_entry FILENAME _package_name)
  string(TOUPPER "${_package_name}" _package_name_upper)
  if(NOT _package_name_upper MATCHES "\\.DLL$")
    continue()
  endif()
  list(FIND _packaged_dll_names_upper "${_package_name_upper}"
    _duplicate_index)
  if(NOT _duplicate_index EQUAL -1)
    message(FATAL_ERROR
      "Runtime package contains case-colliding DLL names: ${_package_name}")
  endif()
  list(APPEND _packaged_dll_names "${_package_name}")
  list(APPEND _packaged_dll_names_upper "${_package_name_upper}")
  list(APPEND _packaged_dll_paths "${_package_entry}")
endforeach()

set(_expected_sorted ${_expected_runtime_dll_names_upper})
set(_packaged_sorted ${_packaged_dll_names_upper})
list(SORT _expected_sorted)
list(SORT _packaged_sorted)
if(NOT "${_packaged_sorted}" STREQUAL "${_expected_sorted}")
  set(_missing_dlls)
  foreach(_dll IN LISTS _expected_runtime_dll_names_upper)
    list(FIND _packaged_dll_names_upper "${_dll}" _dll_index)
    if(_dll_index EQUAL -1)
      list(APPEND _missing_dlls "${_dll}")
    endif()
  endforeach()
  set(_extra_dlls)
  foreach(_dll IN LISTS _packaged_dll_names_upper)
    list(FIND _expected_runtime_dll_names_upper "${_dll}" _dll_index)
    if(_dll_index EQUAL -1)
      list(APPEND _extra_dlls "${_dll}")
    endif()
  endforeach()
  if(_missing_dlls)
    list(JOIN _missing_dlls ", " _missing_text)
  else()
    set(_missing_text "none")
  endif()
  if(_extra_dlls)
    list(JOIN _extra_dlls ", " _extra_text)
  else()
    set(_extra_text "none")
  endif()
  message(FATAL_ERROR
    "Runtime DLL set does not match the generated target manifest. "
    "Missing: ${_missing_text}. Unexpected: ${_extra_text}.")
endif()

file(GLOB _source_catalogs LIST_DIRECTORIES FALSE
  "${TRANSLATION_SOURCE_DIR}/*.cson")
file(GLOB _packaged_catalogs LIST_DIRECTORIES FALSE
  "${TRANSLATION_PACKAGE_DIR}/*.cson")
if(NOT _source_catalogs)
  message(FATAL_ERROR "No source translation catalogs were found")
endif()
set(_source_catalog_names)
foreach(_catalog IN LISTS _source_catalogs)
  cmake_path(GET _catalog FILENAME _catalog_name)
  list(APPEND _source_catalog_names "${_catalog_name}")
endforeach()
set(_packaged_catalog_names)
foreach(_catalog IN LISTS _packaged_catalogs)
  cmake_path(GET _catalog FILENAME _catalog_name)
  list(APPEND _packaged_catalog_names "${_catalog_name}")
endforeach()
list(SORT _source_catalog_names)
list(SORT _packaged_catalog_names)
if(NOT "${_packaged_catalog_names}" STREQUAL "${_source_catalog_names}")
  message(FATAL_ERROR
    "Packaged translation catalogs do not match the source catalog set. "
    "Expected '${_source_catalog_names}', found '${_packaged_catalog_names}'")
endif()

file(GLOB_RECURSE _source_help_files RELATIVE "${HELP_SOURCE_DIR}"
  LIST_DIRECTORIES FALSE "${HELP_SOURCE_DIR}/*")
file(GLOB_RECURSE _packaged_help_files RELATIVE "${HELP_PACKAGE_DIR}"
  LIST_DIRECTORIES FALSE "${HELP_PACKAGE_DIR}/*")
list(SORT _source_help_files)
list(SORT _packaged_help_files)
if(NOT "${_packaged_help_files}" STREQUAL "${_source_help_files}")
  message(FATAL_ERROR
    "Packaged help assets do not match the source asset set. "
    "Expected '${_source_help_files}', found '${_packaged_help_files}'")
endif()
list(FIND _source_help_files "en/index.html" _english_help_index)
if(_english_help_index EQUAL -1)
  message(FATAL_ERROR "The English offline help entry point is missing")
endif()
foreach(_help_relative_path IN LISTS _source_help_files)
  if(_help_relative_path MATCHES "\\.html$")
    file(READ "${HELP_SOURCE_DIR}/${_help_relative_path}" _expected_help_content)
    string(REPLACE "\r\n" "\n" _expected_help_content "${_expected_help_content}")
    string(CONFIGURE "${_expected_help_content}" _expected_help_content @ONLY)
    string(SHA256 _source_help_hash "${_expected_help_content}")
  else()
    file(SHA256 "${HELP_SOURCE_DIR}/${_help_relative_path}" _source_help_hash)
  endif()
  file(SHA256 "${HELP_PACKAGE_DIR}/${_help_relative_path}" _packaged_help_hash)
  if(NOT _packaged_help_hash STREQUAL _source_help_hash)
    message(FATAL_ERROR
      "Packaged help asset differs from its versioned source: ${_help_relative_path}")
  endif()
endforeach()
file(SHA256 "${HELP_LOGO_SOURCE}" _source_help_logo_hash)
file(SHA256 "${HELP_LOGO_PACKAGE}" _packaged_help_logo_hash)
if(NOT _packaged_help_logo_hash STREQUAL _source_help_logo_hash)
  message(FATAL_ERROR "Packaged help logo differs from its source")
endif()
file(SHA256 "${HELP_FAVICON_SOURCE}" _source_help_favicon_hash)
file(SHA256 "${HELP_FAVICON_PACKAGE}" _packaged_help_favicon_hash)
if(NOT _packaged_help_favicon_hash STREQUAL _source_help_favicon_hash)
  message(FATAL_ERROR "Packaged help favicon differs from its source")
endif()
file(SHA256 "${LICENSE_SOURCE}" _source_license_hash)
file(SHA256 "${LICENSE_PACKAGE}" _packaged_license_hash)
if(NOT _packaged_license_hash STREQUAL _source_license_hash)
  message(FATAL_ERROR "Staged license differs from its source")
endif()

set(_system_dlls
  ADVAPI32.DLL
  BCRYPT.DLL
  CABINET.DLL
  CFGMGR32.DLL
  COMBASE.DLL
  COMCTL32.DLL
  COMDLG32.DLL
  CRYPT32.DLL
  D3D11.DLL
  DNSAPI.DLL
  DWMAPI.DLL
  GDI32.DLL
  GDIPLUS.DLL
  IMM32.DLL
  IPHLPAPI.DLL
  KERNEL32.DLL
  KERNELBASE.DLL
  MPR.DLL
  MSIMG32.DLL
  MSVCRT.DLL
  NCRYPT.DLL
  NETAPI32.DLL
  NORMALIZ.DLL
  NTDLL.DLL
  OLE32.DLL
  OLEACC.DLL
  OLEAUT32.DLL
  POWRPROF.DLL
  PROPSYS.DLL
  RPCRT4.DLL
  SECUR32.DLL
  SETUPAPI.DLL
  SHELL32.DLL
  SHLWAPI.DLL
  USER32.DLL
  USERENV.DLL
  UCRTBASE.DLL
  USP10.DLL
  UXTHEME.DLL
  UUID.DLL
  VERSION.DLL
  WINHTTP.DLL
  WININET.DLL
  WINMM.DLL
  WINDOWSCODECS.DLL
  WINSPOOL.DRV
  WLDAP32.DLL
  WS2_32.DLL
  WTSAPI32.DLL
)

function(_havremote_audit_local_paths artifact_path)
  if(ENFORCE_REPRODUCIBLE_PATHS)
    file(STRINGS "${artifact_path}" _embedded_absolute_paths
      REGEX "[A-Za-z]:[/\\\\]")
    foreach(_embedded_path IN LISTS _embedded_absolute_paths)
      string(REPLACE "\\" "/" _embedded_path "${_embedded_path}")
      string(TOLOWER "${_embedded_path}" _embedded_path_folded)
      foreach(_local_prefix IN LISTS _local_build_path_prefixes)
        string(FIND "${_embedded_path_folded}" "${_local_prefix}"
          _local_prefix_index)
        if(NOT _local_prefix_index EQUAL -1)
          cmake_path(GET artifact_path FILENAME _artifact_name)
          message(FATAL_ERROR
            "${_artifact_name} embeds a local source, build, or toolchain path: "
            "${_embedded_path}")
        endif()
      endforeach()
    endforeach()
  endif()
endfunction()

function(_havremote_audit_pe pe_path require_application_resources)
  _havremote_audit_local_paths("${pe_path}")
  if(REQUIRE_DEBUG_FILES)
    set(_debug_file "${pe_path}.debug")
    if(NOT EXISTS "${_debug_file}")
      message(FATAL_ERROR "Detached debug file is missing: ${_debug_file}")
    endif()
    _havremote_audit_local_paths("${_debug_file}")
  endif()

  execute_process(
    COMMAND "${OBJDUMP}" -p "${pe_path}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _headers
    ERROR_VARIABLE _error
  )
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "Could not inspect ${pe_path}: ${_error}")
  endif()

  string(REGEX MATCHALL "DLL Name: *[^\r\n]+" _import_lines "${_headers}")
  set(_imports)
  foreach(_line IN LISTS _import_lines)
    string(REGEX REPLACE "DLL Name: *" "" _dll "${_line}")
    string(STRIP "${_dll}" _dll)
    string(TOUPPER "${_dll}" _dll)
    list(APPEND _imports "${_dll}")
  endforeach()
  list(REMOVE_DUPLICATES _imports)
  list(SORT _imports)

  foreach(_dll IN LISTS _imports)
    if(_dll MATCHES "${_forbidden_runtime_regex}")
      message(FATAL_ERROR
        "${pe_path} imports forbidden GCC/MinGW/MSYS runtime DLL ${_dll}. "
        "The static runtime boundary regressed")
    endif()
    if(_dll MATCHES "^(API-MS-WIN-|EXT-MS-WIN-)")
      continue()
    endif()
    list(FIND _system_dlls "${_dll}" _system_index)
    if(NOT _system_index EQUAL -1)
      continue()
    endif()
    list(FIND _packaged_dll_names_upper "${_dll}" _packaged_index)
    if(_packaged_index EQUAL -1)
      message(FATAL_ERROR
        "${pe_path} imports non-system DLL ${_dll}, but that DLL is not in "
        "the reviewed runtime bundle")
    endif()
  endforeach()

  if(require_application_resources)
    # An application manifest belongs in RT_MANIFEST (resource type 24),
    # numeric resource ID 1.
    string(REGEX MATCH
      "Entry: ID: 0x0*18, Value:[^\r\n]*[\r\n]+[^\r\n]*Name Table:[^\r\n]*[\r\n]+[^\r\n]*Entry: ID: 0x0*1,"
      _manifest_resource "${_headers}")
    if(NOT _manifest_resource)
      message(FATAL_ERROR
        "${pe_path} does not contain RT_MANIFEST resource ID 1")
    endif()

    # The project icon sorts before wxWidgets' fallback icon
    # so Windows Explorer selects it as the executable icon.
    if(NOT _headers MATCHES "HAVREMOTE_APP_ICON")
      message(FATAL_ERROR
        "${pe_path} does not contain the havRemote application icon")
    endif()

    foreach(_embedded_png IN ITEMS
        HAVREMOTE_CLEAR_ICON
        HAVREMOTE_CLEAR_HISTORY_ICON
        HAVREMOTE_HISTORY_ICON
        HAVREMOTE_CONNECT_ICON
        HAVREMOTE_DISCONNECT_ICON
        HAVREMOTE_DOWNLOAD_ICON
        HAVREMOTE_FILE_LIST_FILE_ICON
        HAVREMOTE_FILE_LIST_FOLDER_ICON
        HAVREMOTE_FILE_LIST_SYMLINK_ICON
        HAVREMOTE_FILE_LIST_UNKNOWN_ICON
        HAVREMOTE_NEW_CONNECTION_ICON
        HAVREMOTE_REFRESH_ICON
        HAVREMOTE_SITE_MANAGER_ICON
        HAVREMOTE_UP_ICON
        HAVREMOTE_UPLOAD_ICON)
      if(NOT _headers MATCHES "${_embedded_png}")
        message(FATAL_ERROR
          "${pe_path} does not contain embedded PNG resource ${_embedded_png}")
      endif()
    endforeach()

    # wx/msw/wx.rc supplies the standard resources expected by wxMSW
    if(NOT _headers MATCHES "WXICON_AAA")
      message(FATAL_ERROR
        "${pe_path} does not contain the standard wxWidgets MSW resources")
    endif()
  endif()

  cmake_path(GET pe_path FILENAME _pe_name)
  if(_imports)
    list(JOIN _imports ", " _import_text)
  else()
    set(_import_text "none")
  endif()
  message(STATUS "PE import audit passed for ${_pe_name}: ${_import_text}")
endfunction()

_havremote_audit_pe("${PE_FILE}" TRUE)
foreach(_runtime_dll IN LISTS _packaged_dll_paths)
  _havremote_audit_pe("${_runtime_dll}" FALSE)
endforeach()

list(JOIN _expected_runtime_dll_names ", " _runtime_dll_text)
message(STATUS
  "Runtime bundle, PE imports, detached symbols, local-path hygiene, manifest, "
  "application icon, embedded PNG resources, wxWidgets resource, translation, "
  "offline-help asset, and license audit passed. "
  "Bundled DLLs: "
  "${_runtime_dll_text}")
