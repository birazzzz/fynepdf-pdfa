# Runs veraPDF over a folder of files that say they're PDF/A and fails on any that isn't:
#   cmake -DVERAPDF=<verapdf command> -DFLAVOUR=2b -DDIR=<folder> -P verapdf.cmake
# VERAPDF is the veraPDF command line, or "docker" for its image (ghcr.io/verapdf/cli).
file(GLOB files "${DIR}/*.pdf")
list(LENGTH files count)
if(count EQUAL 0)
    message(FATAL_ERROR "no files in ${DIR}")
endif()
if(VERAPDF STREQUAL "docker")
    set(names "")
    foreach(file ${files})
        get_filename_component(name "${file}" NAME)
        list(APPEND names "/data/${name}")
    endforeach()
    execute_process(COMMAND docker run --rm -v "${DIR}:/data" ghcr.io/verapdf/cli:latest
            --flavour ${FLAVOUR} --format text ${names}
        OUTPUT_VARIABLE output ERROR_QUIET)
else()
    execute_process(COMMAND "${VERAPDF}" --flavour ${FLAVOUR} --format text ${files}
        OUTPUT_VARIABLE output ERROR_QUIET)
endif()
string(REGEX MATCHALL "(^|\n)PASS [^\n]*" passed "${output}")
string(REGEX MATCHALL "(^|\n)FAIL [^\n]*" failed "${output}")
list(LENGTH passed passed_count)
message(STATUS "veraPDF ${FLAVOUR}: ${passed_count} of ${count} pass")
if(failed OR NOT passed_count EQUAL count)
    message(FATAL_ERROR "files labelled PDF/A-${FLAVOUR} that veraPDF fails:${failed}")
endif()
