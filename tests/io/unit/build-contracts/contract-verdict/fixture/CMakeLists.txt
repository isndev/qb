# The fixture of qb-io-test-unit-build-contract-verdict (check.cmake beside it): no language, nothing compiled. Each
# case is a target that prints the output check.cmake planted for it (OUT_DIR/<case>.txt) and fails, as a refused
# unit's build fails; `built` prints its own -- the expected error -- and succeeds, as a unit that lost its refusal
# builds. Not in ALL: the verdict builds one at a time. With CONFIG_CASE, a multi-config fixture holds that one target
# alone, which prints OUT_DIR/<case>-<configuration>.txt: the configurations are CONFIG_TYPES, the default the first.
#   cmake -S fixture -B <dir> -G <generator> -DOUT_DIR=<planted outputs> -DCASES=<case>|<case>|...
#   cmake -S fixture -B <dir> -G "Ninja Multi-Config" -DOUT_DIR=<planted outputs> -DCONFIG_TYPES=<config>|<config>
#         -DCONFIG_CASE=<case>
cmake_minimum_required(VERSION 3.24)
if(CONFIG_TYPES) # before project(), where a multi-config generator reads them
    string(REPLACE "|" ";" _types "${CONFIG_TYPES}")
    set(CMAKE_CONFIGURATION_TYPES "${_types}" CACHE STRING "" FORCE)
endif()
project(contract_verdict_fixture LANGUAGES NONE)

if(NOT OUT_DIR OR (NOT CASES AND NOT CONFIG_CASE))
    message(FATAL_ERROR "contract-verdict fixture: OUT_DIR and CASES or CONFIG_CASE are required")
endif()
if(CONFIG_CASE)
    add_custom_target(${CONFIG_CASE} COMMAND "${CMAKE_COMMAND}" -E cat "${OUT_DIR}/${CONFIG_CASE}-$<CONFIG>.txt"
                                     COMMAND "${CMAKE_COMMAND}" -E false VERBATIM)
    return()
endif()
add_custom_target(built COMMAND "${CMAKE_COMMAND}" -E cat "${OUT_DIR}/built.txt" COMMAND "${CMAKE_COMMAND}" -E true
                  VERBATIM)
string(REPLACE "|" ";" _cases "${CASES}")
foreach(_case IN LISTS _cases)
    add_custom_target(${_case} COMMAND "${CMAKE_COMMAND}" -E cat "${OUT_DIR}/${_case}.txt"
                               COMMAND "${CMAKE_COMMAND}" -E false VERBATIM)
endforeach()
