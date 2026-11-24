foreach(arguments IN ITEMS "--port;65536" "--http-threads;0" "--bind;not-an-address" "--unknown;1")
    execute_process(COMMAND "${SERVER}" ${arguments}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if(NOT result EQUAL 2 OR NOT error MATCHES "Configuration error" OR output MATCHES "server ready")
        message(FATAL_ERROR "Invalid server configuration did not fail safely: ${result}\n${output}\n${error}")
    endif()
endforeach()
execute_process(COMMAND "${SERVER}" --bind 192.0.2.254 --port 0
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT result EQUAL 1 OR NOT error MATCHES "bind validation failed" OR output MATCHES "server ready")
    message(FATAL_ERROR "Unavailable listener did not fail cleanly: ${result}\n${output}\n${error}")
endif()
