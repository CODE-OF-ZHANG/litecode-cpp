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
add_executable(test_rate_limit       unit/test_rate_limit.cpp)
add_executable(test_refresh_token    unit/test_refresh_token.cpp)
add_executable(test_user_repo        unit/test_user_repo.cpp)
add_executable(test_problem          unit/test_problem.cpp)
add_executable(test_auth_register    unit/test_auth_register.cpp)
add_executable(test_auth_login       unit/test_auth_login.cpp)
add_executable(test_auth_refresh     unit/test_auth_refresh.cpp)
add_executable(test_auth_logout      unit/test_auth_logout.cpp)
add_executable(test_auth_profile     unit/test_auth_profile.cpp)
add_executable(test_tag              unit/test_tag.cpp)
add_executable(test_audit_log        unit/test_audit_log.cpp)
add_executable(test_problem_list     unit/test_problem_list.cpp)
add_executable(test_problem_revision unit/test_problem_revision.cpp)
add_executable(test_problem_detail    unit/test_problem_detail.cpp)
add_executable(test_tag_list          unit/test_tag_list.cpp)
add_executable(test_admin_problem_crud unit/test_admin_problem_crud.cpp)
add_executable(test_admin_bulk_import unit/test_admin_bulk_import.cpp)
add_executable(test_docker_client    unit/test_docker_client.cpp)
add_executable(test_warm_pool        unit/test_warm_pool.cpp)
add_executable(test_judge_scheduler  unit/test_judge_scheduler.cpp)
add_executable(test_submission       unit/test_submission.cpp)
add_executable(test_submission_sse   unit/test_submission_sse.cpp)

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

# rate_limit.h (Phase 2 ★) — in-memory token-bucket middleware. Pulls in
# server.h (for ApiException + send_error + the E2E HttpServer) and
# auth_middleware.h (for extract_bearer_token, used by try_extract_jwt_subject
# to key on user_id without verifying the signature). jwt-cpp is in the
# link set because auth_middleware.h transitively includes jwt_utils.h,
# which references the jwt-cpp traits header (even though rate_limit itself
# never verifies signatures — it does best-effort base64 decode only).
target_link_libraries(test_rate_limit PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_rate_limit PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

# refresh_token.h (Phase 2 ★) — refresh-token mechanism (issue / rotate /
# revoke / blacklist). Header-only, depends on jwt_utils.h (which
# transitively pulls in jwt-cpp + OpenSSL HMAC). No httplib / nlohmann_json
# / mysql dependency because the surface is just std::string / Claims.
target_link_libraries(test_refresh_token PRIVATE
    GTest::gtest_main
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_refresh_token PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

# user_repo.h (Phase 2 ★) — INSERT / SELECT / UPDATE on `users`. Pulls in
# password_hash.h (for hash_password in the integration test setup) +
# connection_pool.h (mysqlx::Session). Pure-unit tests don't need a
# server / nlohmann_json / httplib; integration tests do — but since
# the same binary serves both, we link the full stack and skip the
# integration tests when MySQL is unreachable.
target_link_libraries(test_user_repo PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_user_repo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# problem_repo.h (Phase 3 ★) — INSERT / SELECT / UPDATE / soft_delete
# on `problems`. Pulls in connection_pool.h (mysqlx::Session) +
# logger.h (LOG_WARN best-effort). The unit tests are pure C++ (no
# SQL, no bcrypt); the integration tests are gated on MySQL
# availability and are skipped when ping() fails. Same link set as
# test_connection_pool.
target_link_libraries(test_problem PRIVATE
    GTest::gtest_main
    mysql::concpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_problem PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# tag_repo.h (Phase 3 ★) — INSERT / SELECT / UPDATE / DELETE on
# `tags` + M:N helpers on `problem_tags` (attach / detach / clear /
# replace / list_*) + bulk resolver find_or_create_many. The unit
# tests are pure C++; the integration tests spin up real
# ProblemRow rows to exercise the M:N paths. Same link set as
# test_problem.
target_link_libraries(test_tag PRIVATE
    GTest::gtest_main
    mysql::concpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_tag PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# audit_log_repo.h (Phase 3 ★) — record() / record_best_effort /
# record_login_failure writers + find_by_id / list / count readers
# on `audit_logs`. The unit tests are pure C++ (validators +
# exception hierarchy + constants); the integration tests create
# throwaway admin users (so admin_id has a valid FK target) and
# insert / query / cleanup rows. Same link set as test_problem +
# nlohmann_json (payload round-trips as JSON text).
target_link_libraries(test_audit_log PRIVATE
    GTest::gtest_main
    mysql::concpp
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_audit_log PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# problem_routes.h (Phase 3 ★) — GET /api/v1/problems public list
# endpoint. End-to-end tests spin up the full HttpServer +
# ConnectionPool + RateLimiter stack, seed rows via problem_repo
# (and tag_repo for tag-filter cases), and assert the envelope
# shape end-to-end. Same link set as test_audit_log (the handler
# needs both the DB driver for problem_repo and nlohmann_json for
# the envelope), plus httplib for the in-process server. Pure
# unit tests (validators + serialize_problem_row + clamp_pagination)
# run without MySQL — those tests SKIP the integration cases when
# the pool can't be reached.
target_link_libraries(test_problem_list PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_problem_list PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# problem_routes.h (Phase 3 ★ follow-up) — GET /api/v1/problems/:slug
# public detail endpoint (SPEC §5.2, A5). End-to-end tests spin up
# the full HttpServer + ConnectionPool + RateLimiter stack, seed a
# problem via problem_repo, attach tags via tag_repo, and insert
# is_sample=TRUE rows into test_cases directly (no public
# test_case_repo write API yet). The pure-unit tests (validators
# + extract_slug_from_path + serialize_problem_detail) run without
# MySQL. Same link set as test_problem_list.
target_link_libraries(test_problem_detail PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_problem_detail PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# tag_routes.h (Phase 3 ★) — GET /api/v1/tags public list endpoint
# (SPEC §5.2). End-to-end tests spin up the full HttpServer +
# ConnectionPool + RateLimiter stack, seed tags via tag_repo and
# problems via problem_repo (and attach via tag_repo::attach) so
# the problem_count field is exercised end-to-end against MySQL.
# The route handler does NOT call consume_rate_limit() (SPEC §5.2
# puts no quota on /api/v1/tags), but the registration signature
# still takes a RateLimiter so a future "add 60/min/IP quota"
# commit doesn't have to retune every test call site. The pure
# unit tests (serialize_tag_with_count shape / UTF-8 round-trip /
# zero-count handling) run without MySQL. Same link set as
# test_problem_list.
target_link_libraries(test_tag_list PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_tag_list PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# auth_routes.h (Phase 2 ★) — POST /api/v1/auth/register. End-to-end
# tests spin up the full stack: HttpServer + ConnectionPool + RateLimiter
# + JWT signing. Same link set as test_auth_middleware, plus the mysql
# driver (the route handler talks to the DB) and bcrypt (for password
# hashing in the happy-path test).
target_link_libraries(test_auth_register PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_auth_register PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# auth_routes.h's POST /api/v1/auth/login — same shape as
# test_auth_register (full HttpServer + ConnectionPool + RateLimiter +
# JWT stack). The login-failure-tracker unit tests + audit_logs query
# need nothing the test_auth_register stack doesn't already pull in.
target_link_libraries(test_auth_login PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_auth_login PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# auth_routes.h's POST /api/v1/auth/refresh — Phase 2 ★. Same link
# set as test_auth_login; the rotation tests drive the full stack
# (server + pool + store + JWT) and verify the new pair with the
# same jwt_utils helpers used in production.
target_link_libraries(test_auth_refresh PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_auth_refresh PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# auth_routes.h's POST /api/v1/auth/logout — Phase 2 ★. Same link
# set as test_auth_refresh; the integration tests drive the full
# stack (server + pool + store + JWT) so the auth middleware +
# theft-defense check inside revoke_refresh_token is exercised
# end-to-end.
target_link_libraries(test_auth_logout PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_auth_logout PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# auth_routes.h's GET /api/v1/auth/profile — Phase 2 ★. Same link
# set as test_auth_logout; profile is a GET so there's no body
# parser, but the integration tests still need the full stack
# (server + pool + JWT) to exercise the auth middleware +
# find_by_id repo call + envelope shape end-to-end.
target_link_libraries(test_auth_profile PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_auth_profile PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

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
add_test(NAME rate_limit        COMMAND test_rate_limit)
add_test(NAME refresh_token     COMMAND test_refresh_token)
add_test(NAME user_repo         COMMAND test_user_repo)
add_test(NAME problem           COMMAND test_problem)
add_test(NAME auth_register     COMMAND test_auth_register)
add_test(NAME auth_login        COMMAND test_auth_login)
add_test(NAME auth_refresh      COMMAND test_auth_refresh)
add_test(NAME auth_logout       COMMAND test_auth_logout)
add_test(NAME auth_profile      COMMAND test_auth_profile)
add_test(NAME tag               COMMAND test_tag)
add_test(NAME audit_log         COMMAND test_audit_log)
add_test(NAME problem_list      COMMAND test_problem_list)
add_test(NAME problem_revision  COMMAND test_problem_revision)
add_test(NAME problem_detail     COMMAND test_problem_detail)
add_test(NAME tag_list           COMMAND test_tag_list)
add_test(NAME admin_problem_crud COMMAND test_admin_problem_crud)
add_test(NAME admin_bulk_import COMMAND test_admin_bulk_import)
add_test(NAME docker_client      COMMAND test_docker_client)
add_test(NAME warm_pool          COMMAND test_warm_pool)
add_test(NAME judge_scheduler    COMMAND test_judge_scheduler)

# admin_problem_routes.h (Phase 3 ★) — POST/PUT/DELETE
# /api/v1/admin/problems endpoints (SPEC §5.2, A18/A19/A20/A27).
# End-to-end tests spin up HttpServer + ConnectionPool + RateLimiter +
# JWT signing (mint admin + user tokens) + problem_repo / tag_repo /
# test_case_repo / audit_log_repo writes. Same link set as
# test_auth_profile (the admin path uses require_admin + writes to
# audit_logs, mirroring the Phase 2 login-failure path).
target_link_libraries(test_admin_problem_crud PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_admin_problem_crud PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
# admin_problem_routes.h now pulls in problem_revisions_repo.h
# (v1.2.12), which combined with the other Phase 3 repos blows
# past the MSVC COFF section limit (C1128). /bigobj fixes it.
# Same trick is in test_admin_bulk_import and main.cpp below.
if(MSVC)
    target_compile_options(test_admin_problem_crud PRIVATE /bigobj)
endif()

# admin_bulk_import_routes.h (Phase 3 ★) — POST
# /api/v1/admin/problems/import (SPEC §5.2, §8.2, A17, A21, A27).
# End-to-end tests spin up HttpServer + ConnectionPool + RateLimiter +
# JWT signing (mint admin + user tokens) + problem_repo / tag_repo /
# test_case_repo / audit_log_repo writes. Same link set as
# test_admin_problem_crud (the bulk-import path uses require_admin +
# writes to audit_logs + invokes the same repos).
target_link_libraries(test_admin_bulk_import PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_admin_bulk_import PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
# problem_revisions_repo.h (v1.2.12) — record / record_best_effort /
# find_by_id / latest_for_problem / list_for_problem / count_for_problem /
# delete_for_problem on `problem_revisions`. The unit tests are pure
# C++ (validators + exception hierarchy + constants); the integration
# tests create throwaway problems via problem_repo + revisions via
# this repo + clean up via delete_for_problem. Same link set as
# test_audit_log (the JSON snapshot round-trip uses nlohmann_json).
target_link_libraries(test_problem_revision PRIVATE
    GTest::gtest_main
    mysql::concpp
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_problem_revision PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
# admin_bulk_import_routes.h is header-only + inline and pulls in
# problem_routes.h / admin_problem_routes.h / tag_repo.h /
# test_case_repo.h / audit_log_repo.h transitively (via the
# namespace ::detail / ::bulk_import::detail split we use to dodge
# ODR collisions). The MSVC linker hits the COFF section limit
# without /bigobj. Same trick is in litecode.vcxproj for main.cpp.
if(MSVC)
    target_compile_options(test_admin_bulk_import PRIVATE /bigobj)
endif()

# docker_client.h (Phase 4 ★) — thin HTTP wrapper around the Docker
# Engine API (talked via tecnativa socket proxy). The unit tests
# cover URL parsing, JSON schema, exception hierarchy, and validators
# without any daemon. The integration tests spin up a mock httplib
# server in-process to simulate the proxy, so the binary is fully
# reproducible without docker installed. Pulls in system_routes.h for
# HealthService::Probe wiring + config.h for JudgeConfig. system_routes.h
# transitively includes connection_pool.h (the HealthService make_db_probe
# factory takes a ConnectionPool*), so we link mysql::concpp for the
# header path — but no actual SQL is opened.
target_link_libraries(test_docker_client PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_docker_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/judge)

# warm_pool.h (Phase 4 ★) — judge container warm-up pool. Pulls in
# docker_client.h (for CreateOptions + Client) + system_routes.h
# (for HealthService::Probe), both header-only and inline. Same link
# set as test_docker_client; the integration tests reuse the same
# mock httplib server pattern so the binary is fully reproducible
# without docker installed.
target_link_libraries(test_warm_pool PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_warm_pool PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/judge)

# judge_scheduler.h (Phase 4 ★) — async judge pipeline: thread pool +
# task queue + max-concurrent + 30s hard timeout + warm_pool
# integration. The unit tests (status enums, validator, parse helper,
# probe wiring) run without MySQL. The integration tests spin up a
# mock httplib docker proxy + (when MySQL is reachable) a real
# ConnectionPool, and exercise the full mark_running → docker
# create → start → wait → logs → mark_finished path end-to-end.
# Pulls in submission_repo + test_case_repo + problem_repo +
# user_repo for the DB integration tests. Same link set as
# test_warm_pool, plus the bcrypt dep (user_repo::create_user hashes
# a password).
target_link_libraries(test_judge_scheduler PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_judge_scheduler PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/judge)

# submission_routes.h (Phase 4 ★) — POST /api/v1/submissions (async),
# GET /api/v1/submissions/:id, GET /api/v1/submissions (history
# list). End-to-end tests spin up HttpServer + ConnectionPool +
# RateLimiter + JWT signing + a JudgeScheduler wired to a MockDockerProxy
# (so the full async pipeline runs without a docker daemon — workers
# fail-fast to SE for the "queue disabled" cases, and produce AC via
# the mock proxy for the round-trip case). The pure-unit tests
# (validators + serialize_submission_row + extract_id_from_path +
# parse_status_param) run without MySQL. Same link set as
# test_judge_scheduler, plus jwt-cpp (the auth path uses
# require_authentication).
target_link_libraries(test_submission PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_submission PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/judge)
add_test(NAME submission          COMMAND test_submission)

# submission_routes.h (Phase 4 ★ follow-up) — SSE push on submission
# results: GET /api/v1/submissions/sse/:id. The new test binary
# covers the SSE handler + the JudgeNotifier pub/sub channel. It
# uses the same stack as test_submission (full HttpServer +
# ConnectionPool + RateLimiter + JWT signing + JudgeScheduler +
# JudgeNotifier) so the link set is identical. The new header
# judge_notifier.h is header-only + inline (no extra deps).
target_link_libraries(test_submission_sse PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_submission_sse PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/judge)
add_test(NAME submission_sse      COMMAND test_submission_sse)