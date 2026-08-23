cmake_minimum_required(VERSION 3.28)

project(
  smooth-autobrightness-for-gnome
  VERSION 0.5.0
  DESCRIPTION "Low-overhead smooth ambient-light brightness for GNOME"
  LANGUAGES C
)

include(GNUInstallDirs)
include(CTest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_library(sabg_core STATIC
  src/ambient_model.c
  src/display_response.c
  src/frame_scheduler.c
  src/keyboard_model.c
  src/output_quantizer.c
  src/smoother.c
  src/suspend_guard.c
  src/target_hysteresis.c
  src/trajectory.c
  src/user_brightness.c
  src/write_tracker.c
)
target_include_directories(sabg_core PUBLIC include)
target_compile_features(sabg_core PUBLIC c_std_17)
target_compile_options(sabg_core PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)
target_link_libraries(sabg_core PUBLIC m)

add_executable(smooth-autobrightness-for-gnome
  src/apple_als_keepalive.c
  src/daemon.c
)
target_compile_features(smooth-autobrightness-for-gnome PRIVATE c_std_17)
target_compile_definitions(smooth-autobrightness-for-gnome PRIVATE
  _POSIX_C_SOURCE=200809L
  SABG_VERSION="${PROJECT_VERSION}"
)
target_compile_options(smooth-autobrightness-for-gnome PRIVATE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)
target_link_libraries(smooth-autobrightness-for-gnome
  PRIVATE sabg_core PkgConfig::SYSTEMD
)

install(TARGETS smooth-autobrightness-for-gnome
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(PROGRAMS tools/smooth-autobrightness-calibrate
  DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES packaging/systemd/smooth-autobrightness-for-gnome.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user
)

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_executable(test-smoother tests/test_smoother.c)
  target_link_libraries(test-smoother PRIVATE sabg_core)
  add_test(NAME smoother COMMAND test-smoother)

  add_executable(test-ambient-model tests/test_ambient_model.c)
  target_link_libraries(test-ambient-model PRIVATE sabg_core)
  add_test(NAME ambient-model COMMAND test-ambient-model)

  add_executable(test-write-tracker tests/test_write_tracker.c)
  target_link_libraries(test-write-tracker PRIVATE sabg_core)
  add_test(NAME write-tracker COMMAND test-write-tracker)

  add_executable(test-keyboard-model tests/test_keyboard_model.c)
  target_link_libraries(test-keyboard-model PRIVATE sabg_core)
  add_test(NAME keyboard-model COMMAND test-keyboard-model)

  add_executable(test-target-hysteresis tests/test_target_hysteresis.c)
  target_link_libraries(test-target-hysteresis PRIVATE sabg_core)
  add_test(NAME target-hysteresis COMMAND test-target-hysteresis)

  add_executable(test-trajectory tests/test_trajectory.c)
  target_link_libraries(test-trajectory PRIVATE sabg_core)
  add_test(NAME trajectory COMMAND test-trajectory)

  add_executable(test-output-quantizer tests/test_output_quantizer.c)
  target_link_libraries(test-output-quantizer PRIVATE sabg_core)
  add_test(NAME output-quantizer COMMAND test-output-quantizer)

  add_executable(test-frame-scheduler tests/test_frame_scheduler.c)
  target_link_libraries(test-frame-scheduler PRIVATE sabg_core)
  add_test(NAME frame-scheduler COMMAND test-frame-scheduler)

  add_executable(test-suspend-guard tests/test_suspend_guard.c)
  target_link_libraries(test-suspend-guard PRIVATE sabg_core)
  add_test(NAME suspend-guard COMMAND test-suspend-guard)

  add_executable(test-user-brightness tests/test_user_brightness.c)
  target_link_libraries(test-user-brightness PRIVATE sabg_core)
  add_test(NAME user-brightness COMMAND test-user-brightness)

  add_executable(test-display-response tests/test_display_response.c)
  target_link_libraries(test-display-response PRIVATE sabg_core)
  add_test(NAME display-response COMMAND test-display-response)

  add_executable(test-apple-als-keepalive
    src/apple_als_keepalive.c
    tests/test_apple_als_keepalive.c
  )
  target_include_directories(test-apple-als-keepalive PRIVATE src)
  target_compile_features(test-apple-als-keepalive PRIVATE c_std_17)
  target_compile_definitions(test-apple-als-keepalive PRIVATE
    _POSIX_C_SOURCE=200809L
  )
  target_compile_options(test-apple-als-keepalive PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  )
  target_link_libraries(test-apple-als-keepalive PRIVATE PkgConfig::SYSTEMD)
  add_test(NAME apple-als-keepalive COMMAND test-apple-als-keepalive)

  add_test(
    NAME calibration-core
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_calibration.py
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/smooth-autobrightness-calibrate
  )
endif()
