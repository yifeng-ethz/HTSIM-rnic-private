# Frozen-behavior regression for htsim_ss_dragonfly legacy invocations.
#
# Each case replays one pre-existing (wave-18 era) command line twice and
# requires the CSV and stdout bytes to equal the tracked golden capture,
# which was produced by the binary at the pre-load-harness base commit
# (89b7a5a).  This is the machine-enforced form of the rule that every
# existing invocation stays byte-identical while the load-harness
# capabilities stay default-off.  Line endings are normalized before
# comparison so the check holds on Windows text-mode stdio.

foreach(required_variable IN ITEMS
        BINARY WORK_DIR DATACENTER_DIR ARGUMENTS GOLDEN_CSV GOLDEN_STDOUT)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${WORK_DIR}")
separate_arguments(argument_list UNIX_COMMAND "${ARGUMENTS}")

function(read_normalized path out_variable)
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    set(${out_variable} "${contents}" PARENT_SCOPE)
endfunction()

read_normalized("${GOLDEN_CSV}" golden_csv)
read_normalized("${GOLDEN_STDOUT}" golden_stdout)

set(repeat_csv "")
set(repeat_stdout "")
foreach(repeat RANGE 1 2)
    set(out_csv "${WORK_DIR}/repeat_${repeat}.csv")
    file(REMOVE "${out_csv}")
    execute_process(
        COMMAND "${BINARY}" ${argument_list} -out "${out_csv}"
        WORKING_DIRECTORY "${DATACENTER_DIR}"
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE run_stdout
        ERROR_VARIABLE run_stderr)
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR
            "htsim_ss_dragonfly exited ${run_result}\n${run_stdout}\n${run_stderr}")
    endif()
    read_normalized("${out_csv}" run_csv)
    string(REPLACE "\r\n" "\n" run_stdout "${run_stdout}")

    if(NOT run_csv STREQUAL golden_csv)
        message(FATAL_ERROR
            "legacy invocation CSV deviates from the golden capture "
            "(repeat ${repeat}); the default-off contract is broken")
    endif()
    if(NOT run_stdout STREQUAL golden_stdout)
        message(FATAL_ERROR
            "legacy invocation stdout deviates from the golden capture "
            "(repeat ${repeat}); the default-off contract is broken\n"
            "observed:\n${run_stdout}")
    endif()

    if(repeat EQUAL 1)
        set(repeat_csv "${run_csv}")
        set(repeat_stdout "${run_stdout}")
    else()
        if(NOT run_csv STREQUAL repeat_csv OR NOT run_stdout STREQUAL repeat_stdout)
            message(FATAL_ERROR "legacy invocation is not repeat-deterministic")
        endif()
    endif()
endforeach()
