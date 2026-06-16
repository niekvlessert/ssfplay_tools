file(GLOB tracks "${MUSIC_DIR}/*.ssf")
list(LENGTH tracks track_count)
if(NOT track_count EQUAL 30)
  message(FATAL_ERROR "Expected 30 SSF tracks, found ${track_count}")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
foreach(track IN LISTS tracks)
  get_filename_component(name "${track}" NAME_WE)
  set(output "${OUTPUT_DIR}/${name}.smoke-1s.vgm")
  execute_process(
    COMMAND "${SSF2VGM}" --length-ms 1000 "${track}" "${output}"
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Conversion failed for ${track}: ${error}")
  endif()
  file(READ "${output}" magic LIMIT 4 HEX)
  if(NOT magic STREQUAL "56676d20")
    message(FATAL_ERROR "Invalid VGM header for ${track}")
  endif()
endforeach()
