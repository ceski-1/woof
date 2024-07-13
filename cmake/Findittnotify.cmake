# Variables defined:
#  ittnotify_FOUND
#  ittnotify_INCLUDE_DIR
#  ittnotify_LIBRARY

find_package(PkgConfig QUIET)
pkg_check_modules(PC_ittnotify QUIET ittnotify)

find_library(ittnotify_LIBRARY
    NAMES itt
    HINTS "C:\\Program Files (x86)\\Intel\\oneAPI\\vtune\\2024.2\\sdk\\lib64")

find_path(ittnotify_INCLUDE_DIR
    NAMES ittnotify.h
    HINTS "C:\\Program Files (x86)\\Intel\\oneAPI\\vtune\\2024.2\\sdk\\include")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ittnotify
    REQUIRED_VARS ittnotify_LIBRARY ittnotify_INCLUDE_DIR)

if(ittnotify_FOUND)
    if(NOT TARGET ittnotify::itt)
        add_library(ittnotify::itt UNKNOWN IMPORTED)
        set_target_properties(ittnotify::itt PROPERTIES
            IMPORTED_LOCATION "${ittnotify_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ittnotify_INCLUDE_DIR}")
    endif()
endif()

mark_as_advanced(ittnotify_LIBRARY ittnotify_INCLUDE_DIR)
