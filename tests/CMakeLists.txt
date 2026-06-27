# tests/CMakeLists.txt
#
# Unit / integration tests for LiteCode-CPP.
# Included only when LITECODE_BUILD_TESTS=ON (see top-level CMakeLists.txt).
#
# CMAKE_CURRENT_SOURCE_DIR here is ${CMAKE_SOURCE_DIR}/tests, so use
# paths relative to that — not the project root.

# config.h is header-only — both test binaries link nothing beyond gtest_main.
add_executable(test_config           unit/test_config.cpp)
add_executable(test_config_singleton unit/test_config_singleton.cpp)
add_executable(test_logger           unit/test_logger.cpp)

foreach(_tgt test_config test_config_singleton test_logger)
    target_link_libraries(${_tgt} PRIVATE GTest::gtest_main)
    target_include_directories(${_tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endforeach()

add_test(NAME config            COMMAND test_config)
add_test(NAME config_singleton  COMMAND test_config_singleton)
add_test(NAME logger            COMMAND test_logger)