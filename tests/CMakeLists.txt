# tests/CMakeLists.txt
#
# Unit / integration tests for LiteCode-CPP.
# Included only when LITECODE_BUILD_TESTS=ON (see top-level CMakeLists.txt).
#
# CMAKE_CURRENT_SOURCE_DIR here is ${CMAKE_SOURCE_DIR}/tests, so use
# paths relative to that — not the project root.

# config.h / logger.h / connection_pool.h / server.h are header-only — their
# test binaries link nothing beyond gtest_main + mysql::concpp / httplib.
add_executable(test_config           unit/test_config.cpp)
add_executable(test_config_singleton unit/test_config_singleton.cpp)
add_executable(test_logger           unit/test_logger.cpp)
add_executable(test_connection_pool  unit/test_connection_pool.cpp)
add_executable(test_server           unit/test_server.cpp)
add_executable(test_uuid             unit/test_uuid.cpp)
add_executable(test_request_id       unit/test_request_id.cpp)

foreach(_tgt test_config test_config_singleton test_logger test_uuid test_request_id)
    target_link_libraries(${_tgt} PRIVATE GTest::gtest_main)
    target_include_directories(${_tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src)
endforeach()

# server.h pulls in cpp-httplib + nlohmann_json, so the test binary needs
# those targets on top of gtest_main. We don't need a real network port
# to be free across machines — bind_any_port() picks an ephemeral one.
# Threads::Threads comes in transitively via httplib::httplib.
target_link_libraries(test_server PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_server PRIVATE ${CMAKE_SOURCE_DIR}/src)

# request_id.h's apply_request_id_header() takes httplib::Response&;
# so this binary also needs the httplib include path even though it
# doesn't bind a real socket.
target_link_libraries(test_request_id PRIVATE
    GTest::gtest_main
    httplib::httplib
    OpenSSL::SSL
    OpenSSL::Crypto)

# connection_pool.h pulls in mysqlx/xdevapi.h + config.h, so the test
# binary needs mysql::concpp + OpenSSL on top of gtest_main.
target_link_libraries(test_connection_pool PRIVATE
    GTest::gtest_main
    mysql::concpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_connection_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

add_test(NAME config            COMMAND test_config)
add_test(NAME config_singleton  COMMAND test_config_singleton)
add_test(NAME logger            COMMAND test_logger)
add_test(NAME connection_pool   COMMAND test_connection_pool)
add_test(NAME server            COMMAND test_server)
add_test(NAME uuid              COMMAND test_uuid)
add_test(NAME request_id        COMMAND test_request_id)