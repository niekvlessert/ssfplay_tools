execute_process(
  COMMAND "${SSF2WAV}" --length-ms 100 --fade-ms 20 "${INPUT}" "${OUTPUT}"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "ssf2wav failed")
endif()
file(SIZE "${OUTPUT}" size)
math(EXPR expected "44 + 120 * 44100 / 1000 * 4")
if(NOT size EQUAL expected)
  message(FATAL_ERROR "unexpected WAV size: ${size}, expected ${expected}")
endif()
file(READ "${OUTPUT}" header HEX OFFSET 0 LIMIT 12)
if(NOT header STREQUAL "52494646d452000057415645")
  message(FATAL_ERROR "unexpected WAV header: ${header}")
endif()
