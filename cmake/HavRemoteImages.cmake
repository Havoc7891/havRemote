# SPDX-License-Identifier: MIT

function(havremote_generate_images output)
  set(_images
    CLEAR=clear CLEAR_HISTORY=clearhistory CONNECT=connect DISCONNECT=disconnect
    DOWNLOAD=download FILE_LIST_FILE=file FILE_LIST_FOLDER=folder
    FILE_LIST_SYMLINK=symlink FILE_LIST_UNKNOWN=unknown HISTORY=history
    NEW_CONNECTION=plus REFRESH=refresh SITE_MANAGER=sitemanager UP=up UPLOAD=upload
    APP=icons/havRemote)
  set(_header "// Generated from the application's PNG assets.\n#ifndef HAVREMOTE_GENERATED_IMAGES_HPP\n#define HAVREMOTE_GENERATED_IMAGES_HPP\n#include <wx/bitmap.h>\n#include <wx/image.h>\n#include <wx/icon.h>\nnamespace havremote::ui {\ninline wxBitmap LoadUiBitmap(const wxString& name) {\n#ifdef __WXMSW__\nreturn wxBitmap{name, wxBITMAP_TYPE_PNG_RESOURCE};\n#else\n")
  foreach(_image IN LISTS _images)
    string(REPLACE "=" ";" _pair "${_image}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _file)
    set(_path "${PROJECT_SOURCE_DIR}/resources/${_file}.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
    file(READ "${_path}" _bytes HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_bytes}")
    string(APPEND _header "if (name == \"HAVREMOTE_${_name}_ICON\") {\nstatic constexpr unsigned char data[] = {${_bytes}};\nreturn wxBitmap::NewFromPNGData(data, sizeof(data));\n}\n")
  endforeach()
  string(APPEND _header "return {};\n#endif\n}\ninline wxIcon LoadApplicationIcon(const wxSize& size = wxDefaultSize) {\n#ifdef __WXMSW__\nreturn wxIcon{\"HAVREMOTE_APP_ICON\", wxBITMAP_TYPE_ICO_RESOURCE, size.x, size.y};\n#else\nauto bitmap = LoadUiBitmap(\"HAVREMOTE_APP_ICON\");\nif (size.IsFullySpecified())\n{\n  bitmap = wxBitmap{bitmap.ConvertToImage().Scale(size.x, size.y, wxIMAGE_QUALITY_HIGH)};\n}\nwxIcon icon;\nicon.CopyFromBitmap(bitmap);\nreturn icon;\n#endif\n}\n}\n#endif\n")
  # configure_file preserves timestamps when asset bytes are unchanged
  file(WRITE "${output}.tmp" "${_header}")
  configure_file("${output}.tmp" "${output}" COPYONLY)
  file(REMOVE "${output}.tmp")
endfunction()
