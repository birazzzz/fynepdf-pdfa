# Finds Little CMS 2 and defines LCMS2::LCMS2: from its CMake package (vcpkg's), from
# pkg-config, or by its header and library.
if(TARGET LCMS2::LCMS2)
    set(LCMS2_FOUND TRUE)
    return()
endif()

find_package(lcms2 CONFIG QUIET)
foreach(candidate lcms2::lcms2 lcms2)
    if(TARGET ${candidate})
        add_library(LCMS2::LCMS2 INTERFACE IMPORTED)
        set_target_properties(LCMS2::LCMS2 PROPERTIES INTERFACE_LINK_LIBRARIES ${candidate})
        set(LCMS2_FOUND TRUE)
        return()
    endif()
endforeach()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LCMS2 QUIET lcms2)
endif()
find_path(LCMS2_INCLUDE_DIR lcms2.h HINTS ${PC_LCMS2_INCLUDE_DIRS})
find_library(LCMS2_LIBRARY NAMES lcms2 liblcms2 lcms2_static HINTS ${PC_LCMS2_LIBRARY_DIRS})
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LCMS2 REQUIRED_VARS LCMS2_LIBRARY LCMS2_INCLUDE_DIR)
if(LCMS2_FOUND)
    add_library(LCMS2::LCMS2 UNKNOWN IMPORTED)
    set_target_properties(LCMS2::LCMS2 PROPERTIES
        IMPORTED_LOCATION "${LCMS2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LCMS2_INCLUDE_DIR}")
endif()
mark_as_advanced(LCMS2_INCLUDE_DIR LCMS2_LIBRARY)
