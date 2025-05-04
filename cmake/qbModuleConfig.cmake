# qb - Module configuration helper
# This file provides helper functions for configuring qb modules

# Function to configure a qb module
function(qb_configure_module)
    # Parse arguments
    set(options)
    set(oneValueArgs NAME VERSION COMPONENT)
    set(multiValueArgs)
    cmake_parse_arguments(MODULE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    # Validate required arguments
    if(NOT MODULE_NAME)
        message(FATAL_ERROR "qb_configure_module: NAME is required")
    endif()
    if(NOT MODULE_COMPONENT)
        message(FATAL_ERROR "qb_configure_module: COMPONENT is required")
    endif()
    if(NOT MODULE_VERSION)
        set(MODULE_VERSION ${QB_VERSION})
    endif()
    
    # Configure version info
    set_target_properties(${MODULE_NAME} PROPERTIES
        VERSION ${MODULE_VERSION}
        SOVERSION ${QB_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # Setup include directories for both build and install interfaces
    target_include_directories(${MODULE_NAME}
        PUBLIC
        "$<BUILD_INTERFACE:${QB_DIRECTORY}/include>"
        INTERFACE
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )

    # Add module directories to the include path
    target_include_directories(${MODULE_NAME}
        PUBLIC "$<BUILD_INTERFACE:${QB_DIRECTORY}/modules>"
    )
    
    # Set component definition
    target_compile_definitions(${MODULE_NAME}
        PUBLIC
        QB_${MODULE_COMPONENT}_VERSION=${QB_VERSION}
    )
    
    # Setup installation
    if (${QB_PREFIX_UPPER}_INSTALL)
        include(GNUInstallDirs)

        install(TARGETS ${MODULE_NAME}
            EXPORT ${MODULE_NAME}Targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )

        # Generate and install export file
        install(EXPORT ${MODULE_NAME}Targets
            FILE ${MODULE_NAME}Targets.cmake
            NAMESPACE qb::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qb
        )
    endif()
endfunction() 