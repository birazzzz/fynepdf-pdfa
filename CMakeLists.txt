cmake_minimum_required(VERSION 3.16)
project(fynepdf_pdfa
    VERSION 0.1.0
    DESCRIPTION "PDF/A for PDFs, fixed in place, without Ghostscript"
    HOMEPAGE_URL "https://github.com/birazzzz/fynepdf-pdfa"
    LANGUAGES CXX)

# Use it from another CMake project in any of three ways, each giving the target fynepdf::pdfa:
#   find_package(fynepdf_pdfa CONFIG REQUIRED)   after installing it (or through vcpkg.json)
#   FetchContent_Declare(fynepdf_pdfa GIT_REPOSITORY https://github.com/birazzzz/fynepdf-pdfa ...)
#   add_subdirectory(fynepdf-pdfa)
#
# It needs qpdf and a libjpeg API (libjpeg-turbo or MozJPEG); OpenJPEG (JPEG 2000 pictures),
# Little CMS (ICC profiles) and PDFium (PDF/A-1 pages that use transparency) are optional. They
# are found on the system, or a project that builds its own names its targets in
# FYNEPDF_PDFA_QPDF_TARGET, FYNEPDF_PDFA_JPEG_TARGET, FYNEPDF_PDFA_OPENJPEG_TARGET,
# FYNEPDF_PDFA_LCMS_TARGET and FYNEPDF_PDFA_PDFIUM_TARGET (empty for none).

set(FYNEPDF_PDFA_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(FYNEPDF_PDFA_TOP_LEVEL ON)
endif()
option(FYNEPDF_PDFA_CLI "Build the fynepdf-pdfa command line" ${FYNEPDF_PDFA_TOP_LEVEL})
option(FYNEPDF_PDFA_TESTS "Build the tests (ctest)" OFF)
option(FYNEPDF_PDFA_TOOLS "Build the developer probes in tests/" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_library(fynepdf_pdfa STATIC
    src/colour.cpp
    src/flatten.cpp
    src/jpeg.cpp
    src/icc.cpp
    src/jpx.cpp
    src/pdf_function.cpp
    src/appearance.cpp
    src/font_check.cpp
    src/font_cmaps.cpp
    src/font_embed.cpp
    src/font_fix.cpp
    src/font_program.cpp
    src/pdfa.cpp
    src/srgb.cpp
    src/xmp.cpp)
add_library(fynepdf::pdfa ALIAS fynepdf_pdfa)
set_target_properties(fynepdf_pdfa PROPERTIES EXPORT_NAME pdfa OUTPUT_NAME fynepdf_pdfa)
target_compile_features(fynepdf_pdfa PUBLIC cxx_std_17)
target_include_directories(fynepdf_pdfa
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
    PRIVATE src)
# qpdf without its old PointerHolder (std::shared_ptr only), as qpdf 11 and 12 recommend.
target_compile_definitions(fynepdf_pdfa PRIVATE POINTERHOLDER_TRANSITION=4)
if(WIN32)
    target_compile_definitions(fynepdf_pdfa PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(fynepdf_pdfa PRIVATE /W4 /utf-8 /bigobj)
    target_compile_definitions(fynepdf_pdfa PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(fynepdf_pdfa PRIVATE -Wall -Wextra)
endif()

# Installing (and find_package from the install) needs dependencies found here, not targets a
# parent project builds.
set(FYNEPDF_PDFA_CAN_INSTALL ON)
foreach(given QPDF JPEG OPENJPEG LCMS PDFIUM)
    if(FYNEPDF_PDFA_${given}_TARGET)
        set(FYNEPDF_PDFA_CAN_INSTALL OFF)
    endif()
endforeach()

if(NOT FYNEPDF_PDFA_QPDF_TARGET)
    find_package(qpdf CONFIG REQUIRED)
    set(FYNEPDF_PDFA_QPDF_TARGET qpdf::libqpdf)
endif()
if(NOT FYNEPDF_PDFA_JPEG_TARGET)
    find_package(JPEG REQUIRED)
    set(FYNEPDF_PDFA_JPEG_TARGET JPEG::JPEG)
endif()
target_link_libraries(fynepdf_pdfa PUBLIC ${FYNEPDF_PDFA_QPDF_TARGET} PRIVATE ${FYNEPDF_PDFA_JPEG_TARGET})

set(FYNEPDF_PDFA_FOUND_OPENJPEG OFF)
if(NOT DEFINED FYNEPDF_PDFA_OPENJPEG_TARGET)
    find_package(OpenJPEG CONFIG QUIET)
    if(OpenJPEG_FOUND)
        set(FYNEPDF_PDFA_OPENJPEG_TARGET openjp2)
        set(FYNEPDF_PDFA_FOUND_OPENJPEG ON)
    endif()
endif()
if(FYNEPDF_PDFA_OPENJPEG_TARGET)
    target_link_libraries(fynepdf_pdfa PRIVATE ${FYNEPDF_PDFA_OPENJPEG_TARGET})
    target_compile_definitions(fynepdf_pdfa PRIVATE FYNEPDF_PDFA_WITH_OPENJPEG)
    message(STATUS "fynepdf-pdfa: JPEG 2000 through ${FYNEPDF_PDFA_OPENJPEG_TARGET}")
else()
    message(STATUS "fynepdf-pdfa: no OpenJPEG, JPEG 2000 pictures are drawn grey through the wrapper colour space")
endif()

set(FYNEPDF_PDFA_FOUND_LCMS OFF)
if(NOT DEFINED FYNEPDF_PDFA_LCMS_TARGET)
    find_package(LCMS2 QUIET)
    if(LCMS2_FOUND)
        set(FYNEPDF_PDFA_LCMS_TARGET LCMS2::LCMS2)
        set(FYNEPDF_PDFA_FOUND_LCMS ON)
    endif()
endif()
if(FYNEPDF_PDFA_LCMS_TARGET)
    target_link_libraries(fynepdf_pdfa PRIVATE ${FYNEPDF_PDFA_LCMS_TARGET})
    target_compile_definitions(fynepdf_pdfa PRIVATE FYNEPDF_PDFA_WITH_LCMS)
    message(STATUS "fynepdf-pdfa: ICC profiles through ${FYNEPDF_PDFA_LCMS_TARGET}")
else()
    message(STATUS "fynepdf-pdfa: no Little CMS, ICC-based colours by formula")
endif()

# PDFium (optional): PDF/A-1 pages with transparency drawn as pictures. A project that ships
# PDFium links it (FYNEPDF_PDFA_PDFIUM_TARGET). Or FYNEPDF_PDFA_PDFIUM_DIR names a
# pdfium-binaries folder (include/, lib/ or bin/): the library is then loaded only when a page
# is drawn (from FYNEPDF_PDFA_PDFIUM, beside the program, or the system), so other runs don't
# load it; the command line gets a copy beside it.
set(FYNEPDF_PDFA_PDFIUM_FILE "")
if(FYNEPDF_PDFA_PDFIUM_TARGET)
    target_link_libraries(fynepdf_pdfa PRIVATE ${FYNEPDF_PDFA_PDFIUM_TARGET})
    target_compile_definitions(fynepdf_pdfa PRIVATE FYNEPDF_PDFA_WITH_PDFIUM)
    message(STATUS "fynepdf-pdfa: PDF/A-1 transparency flattened through ${FYNEPDF_PDFA_PDFIUM_TARGET}")
elseif(FYNEPDF_PDFA_PDFIUM_DIR)
    target_include_directories(fynepdf_pdfa SYSTEM PRIVATE "${FYNEPDF_PDFA_PDFIUM_DIR}/include")
    target_link_libraries(fynepdf_pdfa PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(fynepdf_pdfa PRIVATE FYNEPDF_PDFA_WITH_PDFIUM FYNEPDF_PDFA_PDFIUM_DLOPEN)
    file(GLOB FYNEPDF_PDFA_PDFIUM_FILE
        "${FYNEPDF_PDFA_PDFIUM_DIR}/lib/libpdfium.so" "${FYNEPDF_PDFA_PDFIUM_DIR}/lib/libpdfium.dylib"
        "${FYNEPDF_PDFA_PDFIUM_DIR}/bin/pdfium.dll")
    message(STATUS "fynepdf-pdfa: PDF/A-1 transparency flattened through PDFium, loaded when needed")
else()
    message(STATUS "fynepdf-pdfa: no PDFium, PDF/A-1 pages with transparency are reported as a problem")
endif()

if(FYNEPDF_PDFA_CLI)
    add_executable(fynepdf-pdfa cli/main.cpp)
    target_link_libraries(fynepdf-pdfa PRIVATE fynepdf::pdfa)
    target_compile_definitions(fynepdf-pdfa PRIVATE FYNEPDF_PDFA_VERSION="${PROJECT_VERSION}")
    if(FYNEPDF_PDFA_PDFIUM_FILE)
        add_custom_command(TARGET fynepdf-pdfa POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FYNEPDF_PDFA_PDFIUM_FILE} "$<TARGET_FILE_DIR:fynepdf-pdfa>")
    endif()
endif()

if(FYNEPDF_PDFA_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FYNEPDF_PDFA_TOOLS)
    add_executable(font_probe tests/font_probe.cpp)
    target_include_directories(font_probe PRIVATE src)
    target_link_libraries(font_probe PRIVATE fynepdf_pdfa)
    target_compile_definitions(font_probe PRIVATE POINTERHOLDER_TRANSITION=4)
endif()

set(FYNEPDF_PDFA_INSTALL_DEFAULT OFF)
if(FYNEPDF_PDFA_TOP_LEVEL AND FYNEPDF_PDFA_CAN_INSTALL)
    set(FYNEPDF_PDFA_INSTALL_DEFAULT ON)
endif()
option(FYNEPDF_PDFA_INSTALL "Install the library, headers, fonts and CMake package"
    ${FYNEPDF_PDFA_INSTALL_DEFAULT})
if(FYNEPDF_PDFA_INSTALL AND FYNEPDF_PDFA_CAN_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(FYNEPDF_PDFA_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/fynepdf_pdfa")
    install(TARGETS fynepdf_pdfa EXPORT fynepdf_pdfaTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    # The substitute fonts (PdfaOptions::fonts), with their licences.
    install(DIRECTORY fonts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/fynepdf-pdfa/fonts)
    install(FILES LICENSE THIRD-PARTY-NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/fynepdf-pdfa)
    if(FYNEPDF_PDFA_CLI)
        install(TARGETS fynepdf-pdfa RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        if(FYNEPDF_PDFA_PDFIUM_FILE)
            install(FILES ${FYNEPDF_PDFA_PDFIUM_FILE} DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    endif()
    install(EXPORT fynepdf_pdfaTargets NAMESPACE fynepdf:: DESTINATION ${FYNEPDF_PDFA_CMAKE_DIR})
    configure_package_config_file(cmake/fynepdf_pdfaConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/fynepdf_pdfaConfig.cmake"
        INSTALL_DESTINATION ${FYNEPDF_PDFA_CMAKE_DIR}
        PATH_VARS CMAKE_INSTALL_DATADIR)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/fynepdf_pdfaConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/fynepdf_pdfaConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/fynepdf_pdfaConfigVersion.cmake"
        cmake/FindLCMS2.cmake
        DESTINATION ${FYNEPDF_PDFA_CMAKE_DIR})
endif()
