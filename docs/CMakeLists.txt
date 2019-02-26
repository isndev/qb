
# Doxygen Build
option(BUILD_DOC "Build Documentation" ON)

find_package(Doxygen)
if(DOXYGEN_FOUND)
    set(DOXYGEN_INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(DOXYGEN_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/build/docs)
    set(BUILD_DOC_DIR ${CMAKE_SOURCE_DIR}/build/docs)
    if(NOT EXISTS ${BUILD_DOC_DIR})
        file(MAKE_DIRECTORY ${BUILD_DOC_DIR})
    endif()


    set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
    configure_file(${DOXYGEN_INPUT_DIR}/header.html.in ${DOXYGEN_OUTPUT_DIR}/header.html @ONLY)
    # copy the files needed by the documentation
    configure_file(${DOXYGEN_INPUT_DIR}/doxygen.css ${DOXYGEN_OUTPUT_DIR}/html/doxygen.css COPYONLY)

    message("Doxygen buld started")
    add_custom_target(${CUBE_PREFIX}-docs ALL
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM)
else(DOXYGEN_FOUND)
    message("Doxygen needs to be installed to generate the documentation.")
endif(DOXYGEN_FOUND)