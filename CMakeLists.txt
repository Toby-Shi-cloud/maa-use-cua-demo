cmake_minimum_required(VERSION 3.20)
project(trycua LANGUAGES CXX OBJCXX)
if(NOT APPLE)
  message(FATAL_ERROR "This runner requires macOS 14 or newer")
endif()

# The current installed package exports MaaUtils with Boost::regex but does
# not discover that dependency itself.
find_package(Boost CONFIG REQUIRED COMPONENTS regex)
find_package(MaaFramework CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(cua-shot src/maafw_main.mm)
set_target_properties(cua-shot PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF
  OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS OFF)
target_link_libraries(cua-shot PRIVATE MaaFramework::MaaFramework Threads::Threads
  "-framework AppKit" "-framework ApplicationServices")
target_compile_options(cua-shot PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME cli_help COMMAND cua-shot --help)
  add_test(NAME reject_legacy_gesture COMMAND cua-shot --gesture)
  set_tests_properties(reject_legacy_gesture PROPERTIES WILL_FAIL TRUE)
  add_test(NAME reject_incomplete_click COMMAND cua-shot --app Arknights --click --x 1)
  set_tests_properties(reject_incomplete_click PROPERTIES WILL_FAIL TRUE)
  add_test(NAME reject_custom_steps COMMAND cua-shot --steps 20)
  set_tests_properties(reject_custom_steps PROPERTIES WILL_FAIL TRUE)
endif()
