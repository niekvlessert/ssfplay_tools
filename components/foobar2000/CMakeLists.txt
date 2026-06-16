cmake_minimum_required(VERSION 3.20)

if(NOT FB2K_SDK_DIR)
  message(FATAL_ERROR "Set FB2K_SDK_DIR to a foobar2000 SDK checkout to build foo_ssfplay")
endif()

add_library(foo_ssfplay SHARED foo_ssfplay.cpp)
target_compile_features(foo_ssfplay PRIVATE cxx_std_17)
target_include_directories(foo_ssfplay PRIVATE
  "${FB2K_SDK_DIR}"
  "${FB2K_SDK_DIR}/foobar2000"
)
target_link_libraries(foo_ssfplay PRIVATE ssfplay::ssfplay)
set_target_properties(foo_ssfplay PROPERTIES
  PREFIX ""
  OUTPUT_NAME "foo_ssfplay"
)

if(MSVC AND SSFPLAY_TOOLS_STATIC_RUNTIME)
  set_property(TARGET foo_ssfplay PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
