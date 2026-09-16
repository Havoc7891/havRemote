# SPDX-License-Identifier: MIT

foreach(_required IN ITEMS
    SOURCE_DIR DESTINATION_DIR ALLOWED_ROOT HAVREMOTE_HELP_VERSION)
  if(NOT DEFINED "${_required}" OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

if(NOT HAVREMOTE_HELP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "HAVREMOTE_HELP_VERSION must be MAJOR.MINOR.PATCH")
endif()

cmake_path(ABSOLUTE_PATH SOURCE_DIR NORMALIZE OUTPUT_VARIABLE _source)
cmake_path(ABSOLUTE_PATH DESTINATION_DIR NORMALIZE OUTPUT_VARIABLE _destination)
cmake_path(ABSOLUTE_PATH ALLOWED_ROOT NORMALIZE OUTPUT_VARIABLE _allowed_root)

if(NOT IS_DIRECTORY "${_source}")
  message(FATAL_ERROR "Help source directory does not exist: ${_source}")
endif()
if(NOT EXISTS "${_source}/en/index.html")
  message(FATAL_ERROR "English help entry point is missing: ${_source}/en/index.html")
endif()

# Help staging replaces the directory to prevent a removed localization from
# lingering in an incremental build. Keep the recursive removal constrained to
# an exact `help` child within this build tree.
cmake_path(GET _destination FILENAME _destination_name)
cmake_path(GET _destination PARENT_PATH _destination_parent)
cmake_path(GET _allowed_root ROOT_PATH _allowed_root_component)
if(NOT _destination_name STREQUAL "help")
  message(FATAL_ERROR
    "Refusing to synchronize a destination not named 'help': ${_destination}")
endif()
if(_allowed_root STREQUAL "" OR _allowed_root STREQUAL _allowed_root_component)
  message(FATAL_ERROR "Refusing to use a filesystem root as ALLOWED_ROOT")
endif()
cmake_path(RELATIVE_PATH _destination_parent
  BASE_DIRECTORY "${_allowed_root}"
  OUTPUT_VARIABLE _relative_parent)
if(IS_ABSOLUTE "${_relative_parent}" OR
   _relative_parent MATCHES "^\\.\\.($|[/\\\\])")
  message(FATAL_ERROR
    "Refusing to synchronize outside the build tree: ${_destination}")
endif()
if(_source STREQUAL _destination OR _source STREQUAL _destination_parent)
  message(FATAL_ERROR "Help source and destination must be separate")
endif()

file(REMOVE_RECURSE "${_destination}")
file(MAKE_DIRECTORY "${_destination}")
file(COPY "${_source}/" DESTINATION "${_destination}")

# Generate versioned HTML with consistent line endings on every build host
file(GLOB_RECURSE _help_pages RELATIVE "${_source}"
  LIST_DIRECTORIES FALSE "${_source}/*.html")
foreach(_help_page IN LISTS _help_pages)
  configure_file("${_source}/${_help_page}" "${_destination}/${_help_page}"
    @ONLY NEWLINE_STYLE UNIX)
endforeach()

file(GLOB_RECURSE _help_files LIST_DIRECTORIES FALSE "${_destination}/*")
list(LENGTH _help_files _help_file_count)
message(STATUS
  "Synchronized ${_help_file_count} help asset(s) to ${_destination}")
