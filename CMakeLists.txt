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

# Same runner as a signed App Sandbox .app bundle, to test game lifecycle from a sandboxed caller.
add_executable(cua-shot-sandboxed MACOSX_BUNDLE src/maafw_main.mm)
set_target_properties(cua-shot-sandboxed PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF
  OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES OBJCXX_EXTENSIONS OFF
  MACOSX_BUNDLE_GUI_IDENTIFIER com.trycua.cua-shot-sandboxed
  MACOSX_BUNDLE_BUNDLE_NAME cua-shot-sandboxed)
target_link_libraries(cua-shot-sandboxed PRIVATE MaaFramework::MaaFramework Threads::Threads
  "-framework AppKit" "-framework ApplicationServices")
target_compile_options(cua-shot-sandboxed PRIVATE -Wall -Wextra -Wpedantic)
set_property(TARGET cua-shot-sandboxed APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/sandbox.entitlements)
add_custom_command(TARGET cua-shot-sandboxed POST_BUILD
  COMMAND codesign --force --sign - --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/resources/sandbox.entitlements
          $<TARGET_BUNDLE_DIR:cua-shot-sandboxed>)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME cli_help COMMAND cua-shot --help)
  add_test(NAME reject_legacy_gesture COMMAND cua-shot --gesture)
  set_tests_properties(reject_legacy_gesture PROPERTIES WILL_FAIL TRUE)
  add_test(NAME reject_incomplete_click COMMAND cua-shot --app Arknights --click --x 1)
  set_tests_properties(reject_incomplete_click PROPERTIES WILL_FAIL TRUE)
  add_test(NAME reject_custom_steps COMMAND cua-shot --steps 20)
  set_tests_properties(reject_custom_steps PROPERTIES WILL_FAIL TRUE)
  add_test(NAME reject_missing_game_path COMMAND cua-shot --start-game)
  set_tests_properties(reject_missing_game_path PROPERTIES WILL_FAIL TRUE)
  add_test(NAME reject_conflicting_game_actions COMMAND cua-shot --start-game Example.app --stop-game Example.app)
  set_tests_properties(reject_conflicting_game_actions PROPERTIES WILL_FAIL TRUE)
  add_test(NAME reject_game_window_selector COMMAND cua-shot --start-game Example.app --app Example)
  set_tests_properties(reject_game_window_selector PROPERTIES WILL_FAIL TRUE)

  # End-to-end game lifecycle on TextEdit, run in order. cua-shot is the unsandboxed control.
  set(e2e_app /System/Applications/TextEdit.app)
  set(sandboxed_cmd $<TARGET_BUNDLE_CONTENT_DIR:cua-shot-sandboxed>/MacOS/cua-shot-sandboxed)
  add_test(NAME e2e_00_cleanup COMMAND cua-shot --stop-game ${e2e_app})
  add_test(NAME e2e_10_sandboxed_start COMMAND ${sandboxed_cmd} --start-game ${e2e_app})
  add_test(NAME e2e_11_sandboxed_stop COMMAND ${sandboxed_cmd} --stop-game ${e2e_app})
  add_test(NAME e2e_20_cleanup COMMAND cua-shot --stop-game ${e2e_app})
  add_test(NAME e2e_30_control_start COMMAND cua-shot --start-game ${e2e_app})
  add_test(NAME e2e_31_control_stop COMMAND cua-shot --stop-game ${e2e_app})
  set_tests_properties(e2e_00_cleanup e2e_10_sandboxed_start e2e_11_sandboxed_stop
    e2e_20_cleanup e2e_30_control_start e2e_31_control_stop PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
  set_tests_properties(e2e_10_sandboxed_start PROPERTIES DEPENDS e2e_00_cleanup)
  set_tests_properties(e2e_11_sandboxed_stop PROPERTIES DEPENDS e2e_10_sandboxed_start)
  set_tests_properties(e2e_20_cleanup PROPERTIES DEPENDS e2e_11_sandboxed_stop)
  set_tests_properties(e2e_30_control_start PROPERTIES DEPENDS e2e_20_cleanup)
  set_tests_properties(e2e_31_control_stop PROPERTIES DEPENDS e2e_30_control_start)
endif()
