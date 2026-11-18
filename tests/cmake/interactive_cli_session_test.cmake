cmake_minimum_required(VERSION 3.20)

file(MAKE_DIRECTORY "${JOBQ_SESSION_TEST_DIR}")

function(run_session name input)
    set(input_file "${JOBQ_SESSION_TEST_DIR}/${name}.txt")
    file(WRITE "${input_file}" "${input}")
    execute_process(
        COMMAND "${JOBQ_CLI}" interactive
        INPUT_FILE "${input_file}"
        OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE result
        TIMEOUT 10
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} failed: ${result}\n${output}\n${error}")
    endif()
    foreach(required IN ITEMS "JobQ Interactive Runtime" "Engine initialized successfully."
                              "Runtime started." "jobq> " "Shutting down JobQ..." "Runtime stopped.")
        string(FIND "${output}" "${required}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "${name} missing ${required}\n${output}")
        endif()
    endforeach()
    string(REGEX MATCHALL "Runtime started\\." starts "${output}")
    string(REGEX MATCHALL "Runtime stopped\\." stops "${output}")
    list(LENGTH starts start_count)
    list(LENGTH stops stop_count)
    if(NOT start_count EQUAL 1 OR NOT stop_count EQUAL 1)
        message(FATAL_ERROR "${name} duplicated its lifecycle")
    endif()
    if(name STREQUAL "commands")
        string(REGEX MATCHALL "Available commands:" help "${output}")
        list(LENGTH help help_count)
        if(NOT help_count EQUAL 2 OR NOT error MATCHES "Unknown command: invalid-command")
            message(FATAL_ERROR "Session failed to continue after an unknown command\n${output}\n${error}")
        endif()
    elseif(NOT error STREQUAL "")
        message(FATAL_ERROR "Unexpected session error: ${error}")
    endif()
endfunction()

run_session(commands "help\ninvalid-command\nhelp\nexit\n")
run_session(eof "")
run_session(quit "quit\n")

execute_process(COMMAND "${JOBQ_CLI}" interactive extra
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 2 OR NOT error MATCHES "does not accept arguments")
    message(FATAL_ERROR "Invalid interactive invocation was not rejected")
endif()

execute_process(COMMAND "${JOBQ_CLI}" interactive --help
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 0 OR NOT output MATCHES "Usage: jobq_cli interactive" OR output MATCHES "Runtime started")
    message(FATAL_ERROR "Interactive help unexpectedly started the Engine")
endif()

execute_process(COMMAND "${JOBQ_CLI}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 2 OR NOT error MATCHES "Missing command")
    message(FATAL_ERROR "Existing missing-command behavior changed")
endif()
