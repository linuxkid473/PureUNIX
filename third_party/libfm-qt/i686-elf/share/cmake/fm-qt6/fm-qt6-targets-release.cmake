#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "fm-qt6" for configuration "Release"
set_property(TARGET fm-qt6 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(fm-qt6 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C;CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libfm-qt6.a"
  )

list(APPEND _cmake_import_check_targets fm-qt6 )
list(APPEND _cmake_import_check_files_for_fm-qt6 "${_IMPORT_PREFIX}/lib/libfm-qt6.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
