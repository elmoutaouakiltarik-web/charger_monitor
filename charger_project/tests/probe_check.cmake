# CTest driver for charger_probe: runs it on the candump fixture and checks the
# key lines. Facts only: the report must contain the measured silence and the
# configured timeouts, and must NOT contain a suggestion.
execute_process(COMMAND ${PROBE} ${FIXTURE}
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "charger_probe exited ${rc}: ${err}")
endif()
foreach(needle
        "recorded     2026-10-18T08:00:00Z .. 2026-10-18T08:25:00Z"
        "frames       2978 in 2978 lines, 1.985 frames/s"
        "0x180  measurement  count=1489"
        "0x181  status  count=1489"
        "observed longest silence 13000000 us (13000.000 ms); your measurement_timeout is 3000 ms; your max_integration_step is 2000 ms"
        "longest silence EXCEEDS the measurement_timeout; it EXCEEDS max_integration_step"
        "note: facts only.")
    string(FIND "${out}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing in charger_probe output: ${needle}\n--- output ---\n${out}")
    endif()
endforeach()
string(TOLOWER "${out}" lower)
foreach(forbidden "recommend" "should set" "we suggest")
    string(FIND "${lower}" "${forbidden}" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR "charger_probe must not suggest thresholds: found '${forbidden}'")
    endif()
endforeach()
# JSON mode is deterministic and parseable by eye: same schema line, same numbers.
execute_process(COMMAND ${PROBE} ${FIXTURE} --json OUTPUT_VARIABLE js RESULT_VARIABLE rc2)
if(NOT rc2 EQUAL 0)
    message(FATAL_ERROR "charger_probe --json exited ${rc2}")
endif()
string(FIND "${js}" "\"schema\":\"charger.probe.v1\"" p1)
string(FIND "${js}" "\"longest_silence_us\":13000000" p2)
if(p1 EQUAL -1 OR p2 EQUAL -1)
    message(FATAL_ERROR "unexpected charger_probe --json output: ${js}")
endif()
# A recording without timestamps cannot be measured: exit code 2.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/untimed.candump" "  can0  180   [8]  80 BB E8 03 00 00 00 00\n")
execute_process(COMMAND ${PROBE} "${CMAKE_CURRENT_BINARY_DIR}/untimed.candump" RESULT_VARIABLE rc3 ERROR_QUIET OUTPUT_QUIET)
if(NOT rc3 EQUAL 2)
    message(FATAL_ERROR "a recording without timestamps must exit 2, got ${rc3}")
endif()
