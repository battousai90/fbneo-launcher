#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libzip::zip" for configuration "Release"
set_property(TARGET libzip::zip APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(libzip::zip PROPERTIES
  IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "zstd::libzstd_shared"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libzip.so.5.5"
  IMPORTED_SONAME_RELEASE "libzip.so.5"
  )

list(APPEND _cmake_import_check_targets libzip::zip )
list(APPEND _cmake_import_check_files_for_libzip::zip "${_IMPORT_PREFIX}/lib/libzip.so.5.5" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
