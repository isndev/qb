# qb - Component configuration
# This file defines dependencies between qb components

# Define component dependencies
set(_qb_core_deps io)
set(_qb_io_deps ev)
set(_qb_ev_deps "")

# Function to recursively collect component dependencies
function(qb_get_component_dependencies component result)
    set(_deps "${_qb_${component}_deps}")
    set(_all_deps "${_deps}")
    
    foreach(_dep ${_deps})
        qb_get_component_dependencies(${_dep} _recursive_deps)
        list(APPEND _all_deps ${_recursive_deps})
    endforeach()
    
    if(_all_deps)
        list(REMOVE_DUPLICATES _all_deps)
    endif()
    
    set(${result} "${_all_deps}" PARENT_SCOPE)
endfunction() 