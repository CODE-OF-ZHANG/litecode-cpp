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
add_executable(test_health           unit/test_health.cpp)
add_executable(test_error_handler    unit/test_error_handler.cpp)
add_executable(test_jwt_utils        unit/test_jwt_utils.cpp)
add_executable(test_password_hash    unit/test_password_hash.cpp)
add_executable(test_auth_middleware  unit/test_auth_middleware.cpp)

foreach(_tgt test_config test_config_singleton test_logger test_uuid test_request_id test_error_handler)
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

# error_handler.h is header-only but takes httplib::Response& /
# nlohmann::json directly (parse_json_body, send_error, ApiException),
# so the test binary needs the same link set as test_request_id plus
# nlohmann_json (server.h re-exports these symbols, which is why
# test_server also pulls in nlohmann_json — same chain).
target_link_libraries(test_error_handler PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)

# jwt_utils.h (Phase 2 ★) uses jwt-cpp + OpenSSL HMAC + picojson.
# No MySQL / httplib dependencies — it's a pure crypto helper.
target_link_libraries(test_jwt_utils PRIVATE
    GTest::gtest_main
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_jwt_utils PRIVATE ${CMAKE_SOURCE_DIR}/src)

# password_hash.h (Phase 2 ★) — bcrypt C library + gtest only. Header-only
# C++ wrapper, no jwt-cpp / httplib / nlohmann_json / mysql dependencies.
# litecode_bcrypt is a STATIC lib in the parent CMakeLists.txt that exposes
# src/auth/bcrypt as a PUBLIC include directory, so we just link it.
target_link_libraries(test_password_hash PRIVATE
    GTest::gtest_main
    litecode_bcrypt)
target_include_directories(test_password_hash PRIVATE ${CMAKE_SOURCE_DIR}/src)

# auth_middleware.h / admin_middleware.h (Phase 2 ★) — pulls in jwt_utils.h
# + server.h (for the E2E in-process tests). Same link set as test_server
# plus jwt-cpp.
target_link_libraries(test_auth_middleware PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_auth_middleware PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

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

# system_routes.h (Phase 1 ★ /api/v1/health) pulls in connection_pool.h
# so the test binary needs the full link set used by test_server + the
# mysql driver. Same include path as test_connection_pool.
target_link_libraries(test_health PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_health PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

add_test(NAME config            COMMAND test_config)
add_test(NAME config_singleton  COMMAND test_config_singleton)
add_test(NAME logger            COMMAND test_logger)
add_test(NAME connection_pool   COMMAND test_connection_pool)
add_test(NAME server            COMMAND test_server)
add_test(NAME uuid              COMMAND test_uuid)
add_test(NAME request_id        COMMAND test_request_id)
add_test(NAME health            COMMAND test_health)
add_test(NAME error_handler     COMMAND test_error_handler)
add_test(NAME jwt_utils         COMMAND test_jwt_utils)
add_test(NAME password_hash     COMMAND test_password_hash)
add_test(NAME auth_middleware   COMMAND test_auth_middleware)