cmake_minimum_required(VERSION 3.28)

project(
  smooth-autobrightness-for-gnome
  VERSION 0.3.1
  DESCRIPTION "Low-overhead smooth ambient-light brightness for GNOME"
  LANGUAGES C
)

include(GNUInstallDirs)
include(CTest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_library(sabg_core STATIC
  src/ambient_model.c
  src/keyboard_model.c
  src/smoother.c
  src/target_hysteresis.c
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
install(FILES packaging/systemd/smooth-autobrightness-for-gnome.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user
)

if(BUILD_TESTING)
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
endif()
