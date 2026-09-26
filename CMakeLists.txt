cmake_minimum_required(VERSION 3.20)
project(trycua LANGUAGES CXX)
if(NOT APPLE)
  message(FATAL_ERROR "The bundled Cua SDK is for macOS. Supply a platform SDK to port this project.")
endif()
find_package(nlohmann_json 3.10 REQUIRED)
find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
find_package(Threads REQUIRED)
set(CUA_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cua" CACHE PATH "Cua C ABI SDK directory")
add_library(cua_sdk SHARED IMPORTED)
set_target_properties(cua_sdk PROPERTIES
  IMPORTED_LOCATION "${CUA_SDK_ROOT}/lib/libcua_driver_sdk.dylib"
  INTERFACE_INCLUDE_DIRECTORIES "${CUA_SDK_ROOT}/include")
add_executable(cua-shot src/main.cpp)
add_executable(live-relay scripts/live_relay.cpp)
target_compile_features(live-relay PRIVATE cxx_std_20)
set_target_properties(live-relay PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(live-relay PRIVATE
  "-framework ApplicationServices" "-framework CoreFoundation")
target_compile_options(live-relay PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(cua-shot SYSTEM PRIVATE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(cua-shot PRIVATE ASIO_STANDALONE)
target_compile_features(cua-shot PRIVATE cxx_std_20)
set_target_properties(cua-shot PROPERTIES CXX_EXTENSIONS OFF INSTALL_RPATH "@loader_path/../lib")
target_link_libraries(cua-shot PRIVATE cua_sdk nlohmann_json::nlohmann_json Threads::Threads
  "-framework ApplicationServices" "-framework CoreFoundation")
target_compile_options(cua-shot PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS cua-shot RUNTIME DESTINATION bin)
install(FILES "${CUA_SDK_ROOT}/lib/libcua_driver_sdk.dylib" DESTINATION lib)

include(CTest)
if(BUILD_TESTING)
  enable_language(OBJCXX)

  add_executable(gesture-smoke scripts/tests/gesture_smoke.mm)
  set_target_properties(gesture-smoke PROPERTIES
    OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS NO)
  target_compile_options(gesture-smoke PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(gesture-smoke PRIVATE
    "-framework AppKit" "-framework ApplicationServices" "-framework CoreFoundation")
  add_test(NAME gesture_smoke COMMAND $<TARGET_FILE:gesture-smoke>)

  add_executable(recording-smoke scripts/tests/recording_smoke.cpp)
  target_compile_features(recording-smoke PRIVATE cxx_std_20)
  set_target_properties(recording-smoke PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options(recording-smoke PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(recording-smoke PRIVATE
    "-framework ApplicationServices" "-framework CoreFoundation")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-output")
  add_test(NAME recording_smoke COMMAND $<TARGET_FILE:recording-smoke>
    "${CMAKE_CURRENT_BINARY_DIR}/test-output/recording-smoke.cgevents")
endif()
