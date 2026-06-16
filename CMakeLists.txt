cmake_minimum_required(VERSION 3.20)

project(ssfplay VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckTypeSize)

option(SSFPLAY_BUILD_SHARED "Build shared ssfplay library" OFF)
option(SSFPLAY_BUILD_CLI "Build ssfplay_cli, ssf2wav, and ssf2vgm tools" ON)
option(SSFPLAY_BUILD_PLAYER "Build ssfplay_cli playback tool" ON)
option(SSFPLAY_TOOLS_STATIC_RUNTIME "Use the static MSVC runtime for command-line tools" ON)
option(SSFPLAY_BUILD_WASM "Build the Emscripten/WASM wrapper" OFF)
option(SSFPLAY_BUILD_FOOBAR2000 "Build the foobar2000 component when an SDK is available" OFF)
set(FB2K_SDK_DIR "" CACHE PATH "Path to the foobar2000 SDK")

if(EMSCRIPTEN)
  add_library(ZLIB::ZLIB INTERFACE IMPORTED)
  target_compile_options(ZLIB::ZLIB INTERFACE "SHELL:-sUSE_ZLIB=1")
  target_link_options(ZLIB::ZLIB INTERFACE "SHELL:-sUSE_ZLIB=1")
  set(HAVE_ICONV 0)
  set(SSFPLAY_CONFIG_FIND_ZLIB "")
  set(SSFPLAY_CONFIG_FIND_ICONV "")
else()
  find_package(ZLIB REQUIRED)
  find_package(Iconv REQUIRED)
  set(HAVE_ICONV 1)
  set(SSFPLAY_CONFIG_FIND_ZLIB "find_dependency(ZLIB)")
  set(SSFPLAY_CONFIG_FIND_ICONV "find_dependency(Iconv)")
endif()
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  find_package(ALSA)
endif()

set(MEDNAFEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mednafen")
set(MEDNAFEN_SRC "${MEDNAFEN_DIR}/src")

if(EMSCRIPTEN)
  set(HAVE_INTTYPES_H 1)
  set(HAVE_LIMITS_H 1)
  set(HAVE_STDINT_H 1)
  set(HAVE_STDLIB_H 1)
  set(HAVE_STRING_H 1)
  set(HAVE_SYS_STAT_H 1)
  set(HAVE_SYS_TYPES_H 1)
  set(HAVE_UNISTD_H 1)
  set(HAVE_SIGLONGJMP 0)
  set(HAVE__LONGJMP 0)
  set(SIZEOF_CHAR 1)
  set(SIZEOF_SHORT 2)
  set(SIZEOF_INT 4)
  set(SIZEOF_LONG 4)
  set(SIZEOF_LONG_LONG 8)
  set(SIZEOF_OFF_T 8)
  set(SIZEOF_PTRDIFF_T 4)
  set(SIZEOF_SIZE_T 4)
  set(SIZEOF_VOID_P 4)
else()
  check_include_file("inttypes.h" HAVE_INTTYPES_H)
  check_include_file("limits.h" HAVE_LIMITS_H)
  check_include_file("stdint.h" HAVE_STDINT_H)
  check_include_file("stdlib.h" HAVE_STDLIB_H)
  check_include_file("string.h" HAVE_STRING_H)
  check_include_file("sys/stat.h" HAVE_SYS_STAT_H)
  check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
  check_include_file("unistd.h" HAVE_UNISTD_H)
  check_symbol_exists(siglongjmp "setjmp.h" HAVE_SIGLONGJMP)
  check_symbol_exists(_longjmp "setjmp.h" HAVE__LONGJMP)
  check_type_size("char" SIZEOF_CHAR LANGUAGE CXX)
  check_type_size("short" SIZEOF_SHORT LANGUAGE CXX)
  check_type_size("int" SIZEOF_INT LANGUAGE CXX)
  check_type_size("long" SIZEOF_LONG LANGUAGE CXX)
  check_type_size("long long" SIZEOF_LONG_LONG LANGUAGE CXX)
  check_type_size("off_t" SIZEOF_OFF_T LANGUAGE CXX)
  check_type_size("ptrdiff_t" SIZEOF_PTRDIFF_T LANGUAGE CXX)
  check_type_size("size_t" SIZEOF_SIZE_T LANGUAGE CXX)
  set(SIZEOF_VOID_P "${CMAKE_SIZEOF_VOID_P}")
endif()

configure_file(cmake/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/config.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/trio")
file(COPY
  "${MEDNAFEN_SRC}/trio/trio.h"
  "${MEDNAFEN_SRC}/trio/triodef.h"
  "${MEDNAFEN_SRC}/trio/trionan.h"
  "${MEDNAFEN_SRC}/trio/triop.h"
  "${MEDNAFEN_SRC}/trio/triostr.h"
  DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/generated/trio"
)

set(SSFPLAY_SOURCES
  src/ssfplay.cpp
  src/mednafen_compat.cpp
  ${MEDNAFEN_SRC}/error.cpp
  ${MEDNAFEN_SRC}/Stream.cpp
  ${MEDNAFEN_SRC}/MemoryStream.cpp
  ${MEDNAFEN_SRC}/FileStream.cpp
  ${MEDNAFEN_SRC}/VirtualFS.cpp
  ${MEDNAFEN_SRC}/PSFLoader.cpp
  ${MEDNAFEN_SRC}/SSFLoader.cpp
  ${MEDNAFEN_SRC}/compress/DecompressFilter.cpp
  ${MEDNAFEN_SRC}/compress/ZLInflateFilter.cpp
  ${MEDNAFEN_SRC}/string/string.cpp
  ${MEDNAFEN_SRC}/hw_cpu/m68k/m68k.cpp
  ${MEDNAFEN_SRC}/resampler/resample.c
  ${MEDNAFEN_SRC}/trio/trio.c
  ${MEDNAFEN_SRC}/trio/trionan.c
  ${MEDNAFEN_SRC}/trio/triostr.c
)

if(SSFPLAY_BUILD_SHARED)
  add_library(ssfplay SHARED ${SSFPLAY_SOURCES})
else()
  add_library(ssfplay STATIC ${SSFPLAY_SOURCES})
endif()
add_library(ssfplay::ssfplay ALIAS ssfplay)

target_compile_features(ssfplay PRIVATE cxx_std_11)
target_compile_definitions(ssfplay PRIVATE
  HAVE_CONFIG_H=1
  MDFN_DISABLE_PICPIE_ERRWARN=1
  MDFN_DISABLE_NO_OPT_ERRWARN=1
)
target_compile_options(ssfplay PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-fsigned-char;-fwrapv;-fno-fast-math>
  $<$<COMPILE_LANGUAGE:CXX>:-fsigned-char;-fwrapv;-fno-fast-math;-fexceptions>
)
target_include_directories(ssfplay
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${MEDNAFEN_DIR}/include"
)
target_link_libraries(ssfplay PRIVATE ZLIB::ZLIB)
if(HAVE_ICONV)
  target_link_libraries(ssfplay PRIVATE Iconv::Iconv)
endif()
if(MSVC AND SSFPLAY_TOOLS_STATIC_RUNTIME)
  set_property(TARGET ssfplay PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_target_properties(ssfplay PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
)

function(ssfplay_configure_tool target)
  target_link_libraries(${target} PRIVATE ssfplay::ssfplay)
  if(MSVC AND SSFPLAY_TOOLS_STATIC_RUNTIME)
    set_property(TARGET ${target} PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  if(APPLE)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  elseif(UNIX)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  endif()
endfunction()

set(SSFPLAY_TOOL_TARGETS)
if(SSFPLAY_BUILD_CLI)
  add_executable(ssf2wav tools/ssf2wav.c)
  ssfplay_configure_tool(ssf2wav)
  list(APPEND SSFPLAY_TOOL_TARGETS ssf2wav)

  add_executable(ssf2vgm tools/ssf2vgm.cpp)
  target_compile_features(ssf2vgm PRIVATE cxx_std_11)
  ssfplay_configure_tool(ssf2vgm)
  target_link_libraries(ssf2vgm PRIVATE ZLIB::ZLIB)
  list(APPEND SSFPLAY_TOOL_TARGETS ssf2vgm)

  if(SSFPLAY_BUILD_PLAYER)
    add_executable(ssfplay_cli tools/ssfplay_cli.cpp)
    target_compile_features(ssfplay_cli PRIVATE cxx_std_11)
    ssfplay_configure_tool(ssfplay_cli)
    if(WIN32)
      target_link_libraries(ssfplay_cli PRIVATE winmm)
    elseif(APPLE)
      target_link_libraries(ssfplay_cli PRIVATE "-framework AudioToolbox")
    elseif(ALSA_FOUND)
      target_compile_definitions(ssfplay_cli PRIVATE SSFPLAY_HAVE_ALSA=1)
      target_include_directories(ssfplay_cli PRIVATE ${ALSA_INCLUDE_DIRS})
      target_link_libraries(ssfplay_cli PRIVATE ${ALSA_LIBRARIES})
    endif()
    list(APPEND SSFPLAY_TOOL_TARGETS ssfplay_cli)
  endif()

  add_custom_target(ssfplay_all_tools DEPENDS ${SSFPLAY_TOOL_TARGETS})
endif()

if(SSFPLAY_BUILD_WASM)
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "SSFPLAY_BUILD_WASM requires configuring with emcmake/emcc")
  endif()
  add_executable(ssfplay_wasm src/wasm/ssfplay_wasm.cpp)
  target_compile_features(ssfplay_wasm PRIVATE cxx_std_11)
  ssfplay_configure_tool(ssfplay_wasm)
  target_link_libraries(ssfplay_wasm PRIVATE ZLIB::ZLIB)
  target_link_options(ssfplay_wasm PRIVATE
    "SHELL:-sMODULARIZE=1"
    "SHELL:-sEXPORT_NAME=createSSFPlay"
    "SHELL:-sENVIRONMENT=web,worker,node"
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sEXPORTED_FUNCTIONS=['_malloc','_free','_ssfplay_wasm_open','_ssfplay_wasm_render','_ssfplay_wasm_close','_ssfplay_wasm_last_error']"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['FS','cwrap','ccall','HEAP16','HEAPU8','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
  )
  set_target_properties(ssfplay_wasm PROPERTIES OUTPUT_NAME "ssfplay" SUFFIX ".js")
endif()

if(SSFPLAY_BUILD_FOOBAR2000)
  add_subdirectory(components/foobar2000)
endif()

if(BUILD_TESTING)
  add_executable(ssfplay_tests tests/ssfplay_tests.c)
  target_link_libraries(ssfplay_tests PRIVATE ssfplay::ssfplay)
  add_test(NAME ssfplay_no_fixture_test COMMAND ssfplay_tests)
  set(SSFPLAY_NIGHTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Nights Into Dreams (EMU).zophar")
  set(SSFPLAY_NIGHTS_TRACK "${SSFPLAY_NIGHTS_DIR}/25 Twin Seeds - Growing Wings.ssf")
  if(EXISTS "${SSFPLAY_NIGHTS_TRACK}")
    add_test(NAME ssfplay_tests COMMAND ssfplay_tests "${SSFPLAY_NIGHTS_DIR}")
  else()
    message(STATUS "NiGHTS SSF fixture not found; skipping sample-dependent tests")
  endif()
  if(SSFPLAY_BUILD_CLI AND EXISTS "${SSFPLAY_NIGHTS_TRACK}")
    add_test(NAME ssf2wav_test COMMAND "${CMAKE_COMMAND}"
      "-DSSF2WAV=$<TARGET_FILE:ssf2wav>"
      "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/Nights Into Dreams (EMU).zophar/25 Twin Seeds - Growing Wings.ssf"
      "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/test.wav"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_wav.cmake")
    add_test(NAME ssf2vgm_generate COMMAND ssf2vgm
      --length-ms 1000
      --report "${CMAKE_CURRENT_BINARY_DIR}/test-vgm-report.json"
      "${CMAKE_CURRENT_SOURCE_DIR}/Nights Into Dreams (EMU).zophar/25 Twin Seeds - Growing Wings.ssf"
      "${CMAKE_CURRENT_BINARY_DIR}/test.vgm")
    set_tests_properties(ssf2vgm_generate PROPERTIES FIXTURES_SETUP generated_vgm)
    add_test(NAME ssf2vgm_generate_vgz COMMAND ssf2vgm
      --length-ms 1000
      "${CMAKE_CURRENT_SOURCE_DIR}/Nights Into Dreams (EMU).zophar/25 Twin Seeds - Growing Wings.ssf"
      "${CMAKE_CURRENT_BINARY_DIR}/test.vgz")
    add_executable(vgm_replay_tests tests/vgm_replay_tests.cpp)
    target_compile_features(vgm_replay_tests PRIVATE cxx_std_11)
    target_link_libraries(vgm_replay_tests PRIVATE ssfplay::ssfplay)
    add_test(NAME vgm_replay_tests COMMAND vgm_replay_tests
      "${CMAKE_CURRENT_SOURCE_DIR}/Nights Into Dreams (EMU).zophar/25 Twin Seeds - Growing Wings.ssf"
      "${CMAKE_CURRENT_BINARY_DIR}/test.vgm")
    set_tests_properties(vgm_replay_tests PROPERTIES FIXTURES_REQUIRED generated_vgm)
    add_test(NAME ssf2vgm_all_tracks COMMAND "${CMAKE_COMMAND}"
      "-DSSF2VGM=$<TARGET_FILE:ssf2vgm>"
      "-DMUSIC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/Nights Into Dreams (EMU).zophar"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke-test-vgm-1s"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/convert_all_ssf.cmake")
  endif()
endif()

install(TARGETS ssfplay
  EXPORT ssfplayTargets
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT dev
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT dev
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT dev
)
if(SSFPLAY_BUILD_CLI)
  install(TARGETS ${SSFPLAY_TOOL_TARGETS} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT tools)
endif()
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT dev)
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT tools)
install(FILES mednafen/COPYING
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
  RENAME COPYING.mednafen
  COMPONENT tools
)
install(EXPORT ssfplayTargets
  FILE ssfplayTargets.cmake
  NAMESPACE ssfplay::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ssfplay"
  COMPONENT dev
)
configure_package_config_file(cmake/ssfplayConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/ssfplayConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ssfplay"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ssfplayConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ssfplayConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ssfplayConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ssfplay"
  COMPONENT dev
)

set(CPACK_PACKAGE_NAME "ssfplay-tools")
set(CPACK_PACKAGE_VENDOR "ssfplay")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "ssfplay-tools-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
  set(CPACK_GENERATOR "ZIP")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL tools)
set(CPACK_COMPONENT_TOOLS_DISPLAY_NAME "ssfplay command-line tools")
include(CPack)
