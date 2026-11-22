# qb-io-test-unit-build-contract-verdict (Huly QB-1248): the build contracts' verdict, cmake/check_build_contract.cmake,
# judged on outputs planted here -- nothing compiled. A build contract passes when the verdict accepts its build: a
# verdict that accepted a unit that builds, a second error, another reason or an output it read nothing of would turn
# every contract green, and no contract could tell. Each case is a target of the fixture beside this file that prints
# its planted output and fails (`built` prints the expected error and succeeds); the verdict judges it as it judges a
# contract's, through a nested `cmake --build`, and the case passes only when the verdict says what it must, for the
# reason it must, about the output planted for it (a line naming the case, which the verdict prints with the output):
# - accepted: the one error the message matches, in GCC's form; coloured, as GCC writes it under Make or a forced Ninja
#   (the escape bytes must reach the verdict); MSVC's (`error C2338`); beside the instantiation context (of a type
#   named `ns::error::probe`), a note and a source excerpt whose code holds a label `error:`; ending with a backslash,
#   a note after it;
# - refused, each for its own reason: a target that builds, the expected error in its output (BUILT); the message
#   twice, or beside another error -- a plain one, one holding an unbalanced `]`, one after a template context holding
#   `[with T = int]`, a `fatal error`, one after a note that ends with a backslash, one holding `operator|`, GCC's
#   `sorry, unimplemented`, an `error:` that starts its line (the count);
#   another error alone, the message in a note beside another error, or only in a source excerpt -- code that, read as
#   a diagnostic, would be the error -- beside another (another reason); a failure that printed no error line, the
#   message as a warning, a `#warning` whose text holds `error:`, a severity word in a language the verdict does not
#   read, alone or beside a note and a remark whose text holds `; error:` and `error:` after their own word -- a
#   line's first diagnostic word is its severity, and a `;` splits no line (no error line read: a red the log
#   explains, never a pass);
# - built in the configuration it is asked (--config): in a multi-config fixture (Ninja Multi-Config: this tree's Ninja,
#   else one on the PATH) a target printing the expected error in the configuration asked and another in the default
#   one is accepted when that configuration is asked, and refused for the other error when none is (the premise).
# Every output planted is judged, and only those.
# The verdict's LANGUAGE=C cannot be held by a planted output (none is translated by it).
# The fixture's generator: this build's when it is Ninja or GNU make (their `FAILED:` and `*** [...] Error 1` are no
# error line); otherwise a Ninja found on the PATH -- MSBuild adds `error MSB8066` and NMake `fatal error U1077` to a
# custom command that fails (measured), and no other tool's output is taken to add none -- or none, and the entry is
# skipped and says so. CLICOLOR_FORCE=1 is set here, as the contracts' entries set it: without it Ninja strips the
# colours the coloured case plants. A multi-config fixture gets the one configuration CONFIG names.
#
#   cmake -DVERDICT=<qb>/cmake/check_build_contract.cmake -DWORK_DIR=<scratch directory> -DGENERATOR=<this build's>
#         [-DPLATFORM=..] [-DTOOLSET=..] [-DMAKE_PROGRAM=..] [-DCONFIG=..] -P check.cmake

cmake_minimum_required(VERSION 3.24)

foreach(_required VERDICT WORK_DIR GENERATOR)
    if(NOT ${_required})
        message(FATAL_ERROR "build-contract-verdict: ${_required} is required")
    endif()
endforeach()
cmake_path(IS_ABSOLUTE WORK_DIR _bcv_absolute) # strict: `D:rel`, `/rel` on Windows and `~/rel` are not
if(NOT _bcv_absolute) # the fixture reads OUT_DIR from its own build directory
    message(FATAL_ERROR "build-contract-verdict: WORK_DIR must be an absolute path (${WORK_DIR})")
endif()
# What the entry made there before, and nothing else: WORK_DIR may be a directory a hand run names.
file(REMOVE_RECURSE "${WORK_DIR}/out" "${WORK_DIR}/build" "${WORK_DIR}/build-configs" "${WORK_DIR}/out.path")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
unset(ENV{CMAKE_TOOLCHAIN_FILE}) # every fresh build tree takes it (3.21 on): the fixture has no language to cross-build
set(ENV{CLICOLOR_FORCE} 1)

set(_gen_args)
set(_fixture_generator "${GENERATOR}")
if(GENERATOR MATCHES "^(Ninja|Ninja Multi-Config|Unix Makefiles|MinGW Makefiles|MSYS Makefiles)$")
    list(APPEND _gen_args -G "${GENERATOR}")
    if(PLATFORM)
        list(APPEND _gen_args -A "${PLATFORM}")
    endif()
    if(TOOLSET)
        list(APPEND _gen_args -T "${TOOLSET}")
    endif()
    if(MAKE_PROGRAM)
        list(APPEND _gen_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
    endif()
    if(GENERATOR STREQUAL "Ninja Multi-Config" AND CONFIG)
        list(APPEND _gen_args "-DCMAKE_CONFIGURATION_TYPES=${CONFIG}") # a configuration the defaults may not hold
    endif()
else()
    find_program(_bcv_ninja NAMES ninja ninja-build)
    set(_bcv_runs 1)
    if(_bcv_ninja)
        execute_process(COMMAND "${_bcv_ninja}" --version RESULT_VARIABLE _bcv_runs OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(NOT _bcv_ninja OR NOT _bcv_runs EQUAL 0)
        message("build-contract-verdict: SKIPPED -- the fixture is built with Ninja or GNU make (the ${GENERATOR} "
                "generator's tool is not known to add no error line of its own to a failed command), and no Ninja "
                "that runs is on the PATH")
        return()
    endif()
    list(APPEND _gen_args -G Ninja "-DCMAKE_MAKE_PROGRAM=${_bcv_ninja}")
    set(_fixture_generator "Ninja, found on the PATH (the ${GENERATOR} generator's tool is not used)")
endif()

# The planted outputs, each led by a line naming its case. MESSAGE is matched as a regular expression by the verdict:
# plain words.
set(_msg "the one message this contract expects")
set(_out "${WORK_DIR}/out")
string(ASCII 27 _esc)
set(_expected "unit.cpp:5:3: error: static assertion failed: ${_msg}\n")
set(_accepted one coloured msvc context trailing)
set(_refused twice beside bracket instance fatal continued piped sorry bare other noted quoted silent warned hashwarn
             semi translated)
# Each file written from quoted arguments alone: a list would drop the `;` and the brackets would hold it.
set(_c "contract-verdict case")
file(WRITE "${_out}/one.txt" "${_c} one\n" "${_expected}")
file(WRITE "${_out}/coloured.txt" "${_c} coloured\n"
     "${_esc}[01m${_esc}[Kunit.cpp:5:3:${_esc}[m${_esc}[K ${_esc}[01;31m${_esc}[Kerror: ${_esc}[m${_esc}[K"
     "static assertion failed: ${_msg}\n")
file(WRITE "${_out}/msvc.txt" "${_c} msvc\n" "unit.cpp(5,3): error C2338: static_assert failed: '${_msg}'\n")
file(WRITE "${_out}/context.txt" "${_c} context\n" "unit.cpp: In instantiation of 'struct ns::error::probe<int>':\n"
     "unit.cpp:8:16:   required from here\n" "${_expected}" "    5 |   retry: error: goto retry\n" "      |   ^~~~~\n"
     "unit.cpp:5:3: note: the expression evaluates to false\n")
file(WRITE "${_out}/trailing.txt" "${_c} trailing\n" "unit.cpp:5:3: error: static assertion failed: ${_msg} \\\n"
     "unit.cpp:3:9: note: in expansion of macro 'REFUSE'\n")
file(WRITE "${_out}/built.txt" "${_c} built\n" "${_expected}")
file(WRITE "${_out}/twice.txt" "${_c} twice\n" "${_expected}" "${_expected}")
file(WRITE "${_out}/beside.txt" "${_c} beside\n" "${_expected}"
     "unit.cpp:9:1: error: expected unqualified-id before 'return'\n")
file(WRITE "${_out}/bracket.txt" "${_c} bracket\n" "unit.cpp:4:12: error: expected ']' before ';' token\n"
     "${_expected}")
file(WRITE "${_out}/instance.txt" "${_c} instance\n"
     "unit.cpp: In instantiation of 'struct probe<T> [with T = int]':\n" "unit.cpp:8:16:   required from here\n"
     "${_expected}" "unit.cpp:9:1: error: expected primary-expression before 'return'\n")
file(WRITE "${_out}/fatal.txt" "${_c} fatal\n" "${_expected}"
     "unit.cpp:7:10: fatal error: missing.h: No such file or directory\n")
file(WRITE "${_out}/continued.txt" "${_c} continued\n" "${_expected}"
     "unit.cpp:3:9: note: in expansion of macro 'REFUSE' \\\n"
     "unit.cpp:9:1: error: expected primary-expression before 'return'\n")
file(WRITE "${_out}/piped.txt" "${_c} piped\n" "${_expected}"
     "unit.cpp:7:5: error: no match for 'operator|' (operand types are 'flags' and 'int')\n")
file(WRITE "${_out}/sorry.txt" "${_c} sorry\n" "${_expected}"
     "unit.cpp:7:5: sorry, unimplemented: non-trivial designated initializers not supported\n")
file(WRITE "${_out}/bare.txt" "${_c} bare\n" "${_expected}" "error: the toolchain stopped\n")
file(WRITE "${_out}/other.txt" "${_c} other\n" "unit.cpp:5:3: error: 'timer' was not declared in this scope\n")
file(WRITE "${_out}/noted.txt" "${_c} noted\n" "unit.cpp:2:8: note: ${_msg}\n"
     "unit.cpp:9:1: error: expected primary-expression before 'return'\n")
file(WRITE "${_out}/quoted.txt" "${_c} quoted\n" "unit.cpp:9:1: error: no member named 'timer' in 'session'\n"
     "    5 |   fail: error: ${_msg}\n" "      |   ^~~~\n")
file(WRITE "${_out}/silent.txt" "${_c} silent\n" "the build stopped: subcommand failed.\n")
file(WRITE "${_out}/warned.txt" "${_c} warned\n" "unit.cpp:5:3: warning: ${_msg}\n")
file(WRITE "${_out}/hashwarn.txt" "${_c} hashwarn\n" "unit.cpp:5:3: warning: #warning error: ${_msg} [-Wcpp]\n")
file(WRITE "${_out}/translated.txt" "${_c} translated\n" "unit.cpp:5:3: erreur: static assertion failed: ${_msg}\n")
file(WRITE "${_out}/semi.txt" "${_c} semi\n" "unit.cpp:5:3: erreur: static assertion failed: ${_msg}\n"
     "unit.cpp:3:9: note: in expansion of macro 'REFUSE'; error: ${_msg}\n"
     "unit.cpp:3:9: remark: not inlined: error: ${_msg}\n")

set(_cases ${_accepted} ${_refused})
list(JOIN _cases "|" _case_arg)
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/fixture" -B "${WORK_DIR}/build" ${_gen_args}
                        --no-warn-unused-cli "-DOUT_DIR=${_out}" "-DCASES=${_case_arg}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _log ERROR_VARIABLE _log)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "build-contract-verdict: the fixture did not configure (${_rc}):\n${_log}")
endif()

# What the verdict must say of each case: its own words (matched on its output with every run of blanks made one, so
# that CMake's folding of a long message does not matter); a refusal for another reason names the line it read.
set(_why_accepted "refused, its one error the expected one")
set(_why_built "BUILT -- the refusal it holds is gone")
foreach(_case twice beside bracket instance fatal continued piped sorry bare)
    set(_why_${_case} "failed with 2 errors, one expected")
endforeach()
set(_why_other "was refused for another reason: unit.cpp:5:3: error: 'timer' was not declared in this scope")
set(_why_noted "was refused for another reason: unit.cpp:9:1: error: expected primary-expression before 'return'")
set(_why_quoted "was refused for another reason: unit.cpp:9:1: error: no member named 'timer' in 'session'")
foreach(_case silent warned hashwarn semi translated)
    set(_why_${_case} "no error line was read")
endforeach()

set(_config_arg)
if(CONFIG)
    set(_config_arg "-DCONFIG=${CONFIG}")
endif()
set(_wrong "") # a string, not a list: an output holds `;` and square brackets, which a list would split or keep open
set(_judged)
foreach(_case IN LISTS _accepted _refused ITEMS built)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${WORK_DIR}/build" "-DTARGET=${_case}" ${_config_arg}
                            "-DMESSAGE=${_msg}" -P "${VERDICT}"
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _said ERROR_VARIABLE _said)
    list(APPEND _judged ${_case})
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_said}")
    if(_case IN_LIST _accepted)
        set(_why "${_why_accepted}")
    else()
        set(_why "${_why_${_case}}")
    endif()
    if(_why STREQUAL "") # string(FIND) finds an empty string anywhere: a case with no reason would pass any refusal
        message(FATAL_ERROR "build-contract-verdict: the case ${_case} has no reason to be judged by")
    endif()
    string(FIND "${_flat}" "${_why}" _says_why)
    string(FIND "${_said}" "contract-verdict case ${_case}" _saw_planted)
    if(_saw_planted EQUAL -1)
        string(APPEND _wrong "\n${_case}: its planted output never reached the verdict (rc ${_rc}):\n${_said}")
    elseif(_case IN_LIST _accepted AND NOT _rc EQUAL 0)
        string(APPEND _wrong "\n${_case} (to be accepted) -- refused, rc ${_rc}:\n${_said}")
    elseif(NOT _case IN_LIST _accepted AND _rc EQUAL 0)
        string(APPEND _wrong "\n${_case} (to be refused: ${_why}) -- ACCEPTED:\n${_said}")
    elseif(_says_why EQUAL -1)
        string(APPEND _wrong "\n${_case} (to be judged: ${_why}) -- judged otherwise, rc ${_rc}:\n${_said}")
    endif()
    if(_case STREQUAL "coloured") # the planted sequence itself: Ninja colours its own `FAILED:` under CLICOLOR_FORCE
        string(FIND "${_said}" "${_esc}[01;31m${_esc}[Kerror" _saw_colour)
        if(_saw_colour EQUAL -1)
            string(APPEND _wrong "\ncoloured: its colours did not reach the verdict -- the strip was not exercised")
        endif()
    endif()
endforeach()
# Every output planted was judged, and every case judged was planted: a case dropped from the lists above, its output
# and its reason kept, would otherwise go unnoticed (the third review of this entry).
# The directory's own `[`, `*` and `?` -- a build path may hold them -- are escaped, or the glob reads them as a pattern
# and finds nothing (the fourth review: a binary directory holding `[x]`). The path globbed is the one the outputs were
# written through, as the system resolved it (on POSIX the directory's physical path), which a child cmake run in it
# reads as its working directory and writes to a file (cwd.cmake): nothing of it is decoded on the way (a console code
# page mangled a path holding `é` read from the child's output: the seventh review). file(TO_CMAKE_PATH) splits a POSIX
# path at `:` (the fifth); the glob walks no `.` or `..` that follows a pattern character; cmake_path(NORMALIZE), and
# file(REAL_PATH) under the policies of 3.24 (CMP0152 is 3.28's), collapse a `<symlink>/..` before resolving the
# symlink, elsewhere than the kernel (the sixth).
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=PWD --
                        "${CMAKE_COMMAND}" "-DFILE=${WORK_DIR}/out.path" -P "${CMAKE_CURRENT_LIST_DIR}/cwd.cmake"
                WORKING_DIRECTORY "${_out}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _log ERROR_VARIABLE _log)
set(_out_cmake "")
if(_rc EQUAL 0 AND EXISTS "${WORK_DIR}/out.path")
    file(READ "${WORK_DIR}/out.path" _out_cmake)
endif()
if(NOT _rc EQUAL 0 OR NOT IS_DIRECTORY "${_out_cmake}")
    message(FATAL_ERROR "build-contract-verdict: the output directory's physical path was not read (${_rc}): "
                        "'${_out_cmake}'\n${_log}")
endif()
string(REGEX REPLACE "([[*?])" "[\\1]" _out_glob "${_out_cmake}")
file(GLOB _planted RELATIVE "${_out_cmake}" "${_out_glob}/*.txt")
list(TRANSFORM _planted REPLACE "\\.txt$" "")
list(SORT _planted)
set(_judged_sorted ${_judged})
list(SORT _judged_sorted)
if(NOT _accepted OR NOT _refused OR NOT "${_planted}" STREQUAL "${_judged_sorted}")
    message(FATAL_ERROR "build-contract-verdict: the cases judged (${_judged_sorted}) are not the outputs planted "
                        "(${_planted})")
endif()

# The verdict's --config: a multi-config fixture (Ninja Multi-Config) whose `configured` target prints the expected
# error in the configuration asked, Wanted, and another error in the default one, Plain (the first listed): asked
# Wanted, the verdict must accept it; asked nothing, it must refuse it for that other error -- the premise, without
# which a verdict that dropped --config would pass (the second and third reviews of this entry). The Ninja is this
# tree's own under a Ninja generator, else one found on the PATH; one that does not run, older than Ninja
# Multi-Config's 1.10, or none, leaves the configuration not held, which the last line says.
set(_mc_ninja "")
if(GENERATOR MATCHES "^Ninja" AND MAKE_PROGRAM)
    set(_mc_ninja "${MAKE_PROGRAM}")
else()
    find_program(_bcv_ninja_mc NAMES ninja ninja-build)
    if(_bcv_ninja_mc)
        set(_mc_ninja "${_bcv_ninja_mc}")
    endif()
endif()
set(_config_check "the configuration asked: not held (no Ninja on the PATH)")
if(_mc_ninja)
    execute_process(COMMAND "${_mc_ninja}" --version RESULT_VARIABLE _rc OUTPUT_VARIABLE _mc_version ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
        set(_config_check "the configuration asked: not held (${_mc_ninja} does not run)")
        set(_mc_ninja "")
    elseif(_mc_version VERSION_LESS 1.10)
        set(_config_check "the configuration asked: not held (Ninja ${_mc_version}: Ninja Multi-Config needs 1.10)")
        set(_mc_ninja "")
    endif()
endif()
if(_mc_ninja)
    file(WRITE "${_out}/configured-Wanted.txt" "${_c} configured\n" "${_expected}")
    file(WRITE "${_out}/configured-Plain.txt" "${_c} configured\n"
         "unit.cpp:5:3: error: built in the default configuration, not the one asked\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/fixture" -B "${WORK_DIR}/build-configs"
                            -G "Ninja Multi-Config" "-DCMAKE_MAKE_PROGRAM=${_mc_ninja}" --no-warn-unused-cli
                            "-DOUT_DIR=${_out}" "-DCONFIG_TYPES=Plain|Wanted" -DCONFIG_CASE=configured
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _log ERROR_VARIABLE _log)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "build-contract-verdict: the multi-config fixture did not configure (${_rc}):\n${_log}")
    endif()
    foreach(_asked Wanted default)
        set(_asked_arg)
        set(_why "was refused for another reason: unit.cpp:5:3: error: built in the default configuration")
        if(_asked STREQUAL "Wanted")
            set(_asked_arg -DCONFIG=Wanted)
            set(_why "${_why_accepted}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${WORK_DIR}/build-configs" -DTARGET=configured
                                ${_asked_arg} "-DMESSAGE=${_msg}" -P "${VERDICT}"
                        RESULT_VARIABLE _rc OUTPUT_VARIABLE _said ERROR_VARIABLE _said)
        string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_said}")
        string(FIND "${_flat}" "${_why}" _says_why)
        string(FIND "${_said}" "contract-verdict case configured" _saw_planted)
        if(_saw_planted EQUAL -1 OR _says_why EQUAL -1 OR (_asked STREQUAL "Wanted" AND NOT _rc EQUAL 0))
            string(APPEND _wrong "\nconfigured, asked ${_asked} (to be judged: ${_why}) -- rc ${_rc}:\n${_said}")
        endif()
    endforeach()
    set(_config_check "the configuration asked held (Ninja Multi-Config, ${_mc_ninja})")
endif()

list(LENGTH _accepted _n_accepted)
list(LENGTH _refused _n_refused)
list(LENGTH _judged _n_judged)
if(NOT _wrong STREQUAL "")
    message(FATAL_ERROR "build-contract-verdict: the verdict judged wrong:${_wrong}")
endif()
message(STATUS "build-contract-verdict: ${_n_judged} cases -- ${_n_accepted} accepted, ${_n_refused} refused for their "
               "reason and `built` refused as built, the fixture built with ${_fixture_generator}; ${_config_check}")
