if(NOT DEFINED CASE)
    message(FATAL_ERROR "CASE is required")
endif()

if(CASE STREQUAL "source_txt2bin")
    if(NOT DEFINED SOURCE_TXT2BIN OR NOT DEFINED EXPECTED_SHA256)
        message(FATAL_ERROR
            "SOURCE_TXT2BIN and EXPECTED_SHA256 are required")
    endif()
    if(IS_SYMLINK "${SOURCE_TXT2BIN}")
        file(READ_SYMLINK "${SOURCE_TXT2BIN}" link_target)
        message(FATAL_ERROR
            "the tracked txt2bin source artifact became a symlink to ${link_target}")
    endif()
    if(NOT EXISTS "${SOURCE_TXT2BIN}")
        message(FATAL_ERROR "the tracked txt2bin source artifact is missing")
    endif()
    file(SHA256 "${SOURCE_TXT2BIN}" actual_sha256)
    if(NOT actual_sha256 STREQUAL EXPECTED_SHA256)
        message(FATAL_ERROR
            "the tracked txt2bin source artifact changed: expected ${EXPECTED_SHA256}, got ${actual_sha256}")
    endif()
    return()
endif()

if(CASE STREQUAL "rank_26_round_trip")
    foreach(required_variable IN ITEMS TXT2BIN HTSIM_RNIC WORK_DIR)
        if(NOT DEFINED ${required_variable})
            message(FATAL_ERROR "${required_variable} is required")
        endif()
    endforeach()

    file(MAKE_DIRECTORY "${WORK_DIR}")
    set(goal_path "${WORK_DIR}/rank-26.goal")
    set(binary_path "${WORK_DIR}/rank-26.bin")
    set(completion_path "${WORK_DIR}/rank-26.csv")
    set(goal_text "num_ranks 26\n\n")
    foreach(rank RANGE 0 25)
        string(APPEND goal_text "rank ${rank} {\n")
        if(rank EQUAL 0)
            string(APPEND goal_text "l1: send 1024b to 25 tag 7\n")
        elseif(rank EQUAL 25)
            string(APPEND goal_text "l1: recv 1024b from 0 tag 7\n")
        endif()
        string(APPEND goal_text "}\n\n")
    endforeach()
    file(WRITE "${goal_path}" "${goal_text}")
    file(REMOVE "${binary_path}" "${completion_path}")

    execute_process(
        COMMAND "${TXT2BIN}"
            -i "${goal_path}"
            -o "${binary_path}"
        RESULT_VARIABLE converter_result
        OUTPUT_VARIABLE converter_stdout
        ERROR_VARIABLE converter_stderr)
    if(NOT converter_result EQUAL 0)
        message(FATAL_ERROR
            "txt2bin failed with ${converter_result}\n${converter_stdout}\n${converter_stderr}")
    endif()

    file(READ "${binary_path}" rank_count_hex OFFSET 8 LIMIT 4 HEX)
    if(NOT rank_count_hex STREQUAL "1a000000")
        message(FATAL_ERROR
            "expected little-endian rank count 1a000000, got ${rank_count_hex}")
    endif()

    execute_process(
        COMMAND "${HTSIM_RNIC}"
            -goal "${binary_path}"
            -completion_csv "${completion_path}"
            -goal_rank_mapping gpu-rank
            -linkspeed_bps 400000000000
            -rnic_profile rnic-nn-fluid
        RESULT_VARIABLE simulator_result
        OUTPUT_VARIABLE simulator_stdout
        ERROR_VARIABLE simulator_stderr)
    if(NOT simulator_result EQUAL 0)
        message(FATAL_ERROR
            "htsim_rnic failed with ${simulator_result}\n${simulator_stdout}\n${simulator_stderr}")
    endif()

    string(FIND
        "${simulator_stdout}"
        "requested_rank_mapping=gpu-rank resolved_rank_mapping=gpu-rank ranks=26"
        rank_mapping_position)
    if(rank_mapping_position EQUAL -1)
        message(FATAL_ERROR
            "htsim_rnic did not report the expected 26-rank gpu-rank mapping")
    endif()

    string(FIND
        "${simulator_stdout}"
        "physical_quiescence=verified"
        quiescence_position)
    if(quiescence_position EQUAL -1)
        message(FATAL_ERROR
            "htsim_rnic did not report physical_quiescence=verified")
    endif()

    file(STRINGS "${completion_path}" completion_lines)
    list(LENGTH completion_lines completion_line_count)
    if(NOT completion_line_count EQUAL 2)
        message(FATAL_ERROR
            "expected completion CSV header plus one row, got ${completion_line_count} lines")
    endif()
    list(GET completion_lines 1 completion_row)
    if(NOT completion_row MATCHES "^rnic-nn-fluid,[0-9]+,0,25,7,1024,")
        message(FATAL_ERROR
            "unexpected completion CSV row: ${completion_row}")
    endif()
    return()
endif()

message(FATAL_ERROR "unknown portability regression CASE: ${CASE}")
