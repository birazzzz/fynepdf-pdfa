# ctest: every file in fixtures/ (made for these tests: one PDF/A problem each, and exports from
# Chrome, LibreOffice and LaTeX) converted to PDF/A-1b, 2b and 3b. With
# FYNEPDF_PDFA_VERAPDF set (the veraPDF command line, or "docker" for its image), veraPDF then
# checks every file that says it's PDF/A.
add_executable(fynepdf_pdfa_smoke smoke.cpp)
target_link_libraries(fynepdf_pdfa_smoke PRIVATE fynepdf::pdfa)
target_compile_definitions(fynepdf_pdfa_smoke PRIVATE POINTERHOLDER_TRANSITION=4)

set(FYNEPDF_PDFA_VERAPDF "" CACHE STRING "veraPDF command line, or \"docker\", to check the test results")
set(fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(fonts "${PROJECT_SOURCE_DIR}/fonts")
set(out "${CMAKE_CURRENT_BINARY_DIR}/out")
if(FYNEPDF_PDFA_PDFIUM_TARGET OR FYNEPDF_PDFA_PDFIUM_DIR)
    set(pdfium 1)
else()
    set(pdfium 0)
endif()

foreach(part 1 2 3)
    add_test(NAME pdfa-${part}b
        COMMAND fynepdf_pdfa_smoke ${part} ${fixtures} ${fonts} ${out}/${part}b ${pdfium})
    # PDFium is loaded from beside the program or the system; the command line's copy is beside
    # it in the build folder.
    if(FYNEPDF_PDFA_PDFIUM_FILE)
        set_tests_properties(pdfa-${part}b PROPERTIES ENVIRONMENT "FYNEPDF_PDFA_PDFIUM=${FYNEPDF_PDFA_PDFIUM_FILE}")
    endif()
    if(FYNEPDF_PDFA_VERAPDF)
        set_tests_properties(pdfa-${part}b PROPERTIES FIXTURES_SETUP pdfa-${part}b-files)
        add_test(NAME verapdf-${part}b
            COMMAND ${CMAKE_COMMAND} -DVERAPDF=${FYNEPDF_PDFA_VERAPDF} -DFLAVOUR=${part}b -DDIR=${out}/${part}b
                -P ${CMAKE_CURRENT_SOURCE_DIR}/verapdf.cmake)
        set_tests_properties(verapdf-${part}b PROPERTIES FIXTURES_REQUIRED pdfa-${part}b-files)
    endif()
endforeach()
