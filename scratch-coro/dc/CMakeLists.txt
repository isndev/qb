# Scratch witnesses for the T7 coroutine-teardown confirmation. Never committed.
# dc_probe / dc_exit / dc_ask are the dossier's sources and ch_w / ch_cg the challenger's, copied unchanged; cv_w is
# the confirmation's.
file(READ ${CMAKE_SOURCE_DIR}/src/qb/io/async/coroutine/scheduler.h CV_SCHED_H)
foreach(w dc_probe dc_exit dc_ask ch_w cv_w ch_cg cv_cg cv_ask)
    add_executable(${w} ${CMAKE_CURRENT_SOURCE_DIR}/${w}.cpp)
    target_link_libraries(${w} PRIVATE qb-core)
    qb_apply_compiler_flags(${w})
    if(QB_SANITIZE_LINK_OPTS)
        target_link_options(${w} PRIVATE ${QB_SANITIZE_LINK_OPTS})
    endif()
    if(CV_SCHED_H MATCHES "thread_loop_gone")
        target_compile_definitions(${w} PRIVATE CH_HAS_RECORD)
    endif()
endforeach()

# E5 at the loop: the ev API only
add_executable(e5_ev ${CMAKE_CURRENT_SOURCE_DIR}/e5_ev.cpp)
target_link_libraries(e5_ev PRIVATE qb::ev)
qb_apply_compiler_flags(e5_ev)
if(QB_SANITIZE_LINK_OPTS)
    target_link_options(e5_ev PRIVATE ${QB_SANITIZE_LINK_OPTS})
endif()
