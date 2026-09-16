# SPDX-License-Identifier: MIT

foreach(_required IN ITEMS SOURCE_DIR DESTINATION_DIR ALLOWED_ROOT)
  if(NOT DEFINED "${_required}" OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()

cmake_path(ABSOLUTE_PATH SOURCE_DIR NORMALIZE OUTPUT_VARIABLE _source)
cmake_path(ABSOLUTE_PATH DESTINATION_DIR NORMALIZE OUTPUT_VARIABLE _destination)
cmake_path(ABSOLUTE_PATH ALLOWED_ROOT NORMALIZE OUTPUT_VARIABLE _allowed_root)

if(NOT IS_DIRECTORY "${_source}")
  message(FATAL_ERROR "Translation source directory does not exist: ${_source}")
endif()

# This script performs a stale-safe replacement, so reject every destination
# except an exact `translations` child within this build tree. These guards
# keep REMOVE_RECURSE narrowly scoped even if the command is invoked manually
# with malformed -D arguments.
cmake_path(GET _destination FILENAME _destination_name)
cmake_path(GET _destination PARENT_PATH _destination_parent)
cmake_path(GET _allowed_root ROOT_PATH _allowed_root_component)
if(NOT _destination_name STREQUAL "translations")
  message(FATAL_ERROR
    "Refusing to synchronize a destination not named 'translations': ${_destination}")
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
  message(FATAL_ERROR "Translation source and destination must be separate")
endif()

file(GLOB _catalogs LIST_DIRECTORIES FALSE "${_source}/*.cson")
if(NOT _catalogs)
  message(FATAL_ERROR "No .cson translation catalogs found in ${_source}")
endif()
list(SORT _catalogs)

file(REMOVE_RECURSE "${_destination}")
file(MAKE_DIRECTORY "${_destination}")
file(COPY ${_catalogs} DESTINATION "${_destination}")

list(LENGTH _catalogs _catalog_count)
message(STATUS
  "Synchronized ${_catalog_count} translation catalog(s) to ${_destination}")
