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
add_executable(test_security         integration/test_security.cpp)
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
add_executable(test_special_judge    unit/test_special_judge.cpp)
add_executable(test_warm_pool        unit/test_warm_pool.cpp)
add_executable(test_judge_scheduler  unit/test_judge_scheduler.cpp)
add_executable(test_submission       unit/test_submission.cpp)
add_executable(test_submission_sse   unit/test_submission_sse.cpp)
add_executable(test_auth_cookie_storage unit/test_auth_cookie_storage.cpp)
add_executable(test_stats_profile     unit/test_stats_profile.cpp)
add_executable(test_stats_ranking     unit/test_stats_ranking.cpp)
add_executable(test_admin_users       unit/test_admin_users.cpp)
add_executable(test_admin_stats       unit/test_admin_stats.cpp)
add_executable(test_admin_audit_logs  unit/test_admin_audit_logs.cpp)
add_executable(test_admin_queue       unit/test_admin_queue.cpp)
add_executable(test_admin_special_judge unit/test_admin_special_judge.cpp)
# v1.2.68 Phase 9 ★ Prometheus 指标接入 — MetricsService +
# /api/v1/metrics 路由的 pure-unit + 进程内 httplib smoke。Header-only
# + small surface area, link 集合对齐 test_health (nlohmann_json 不需要，
# 因为 text/plain body 不带 JSON)。
add_executable(test_metrics           unit/test_metrics.cpp)
# v1.2.60 Phase 8 ★ 单元测试覆盖率 ≥ 60%：5 个 pure-unit 测试文件覆盖 repo 层
# validators / filter clamps / build_where_clause — 这些过去没有独立 gtest，
# 只在 route-level 测试里被间接触达。这层新增测试大幅提升 core 覆盖率（auth
# / judge / repo / rate_limit / audit 五块的核心模块），让 scripts/coverage.sh
# gate 在 CI 上能真实 ≥ 60%。
add_executable(test_test_case_repo    unit/test_test_case_repo.cpp)
add_executable(test_submission_repo   unit/test_submission_repo.cpp)
add_executable(test_user_repo_helpers unit/test_user_repo_helpers.cpp)
add_executable(test_problem_repo_helpers unit/test_problem_repo_helpers.cpp)
add_executable(test_audit_log_helpers unit/test_audit_log_helpers.cpp)

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

# test_security.cpp (Phase 6 ★ v1.2.45) — SPEC §15 hardening tests.
# Spins up an in-process HttpServer to assert the SPEC §15 baseline
# response headers (X-Content-Type-Options / X-Frame-Options /
# Referrer-Policy / Permissions-Policy / API-only CSP) ship on every
# response (200/404/preflight). Pure unit tests cover utils/security.h's
# validators (length caps, charset filters, escape helpers, slug /
# username / role shape). Same link set as test_error_handler plus
# a dependency on problem_routes.h (for parse_slug_param) — which
# transitively pulls in the problem_repo header. The integration
# tests don't bind to MySQL; parse_slug_param is exercised without a
# database, mirroring test_problem_detail's pure-unit path.
target_link_libraries(test_security PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_security PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
add_test(NAME security             COMMAND test_security)

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

# special_judge_repo.h (Phase 4 ☆ v1.2.18) — find_by_problem_id /
# exists_for_problem / upsert / remove_by_problem_id on
# `problem_special_judges`. The unit tests are pure C++ (validators +
# constants + exception hierarchy + struct defaults); the integration
# tests create throwaway problems via raw SQL (mirroring
# test_problem_revision's RevisionTracker pattern). Same link set as
# test_audit_log — header-only repo, no MySQL framework deps beyond the
# driver + OpenSSL.
target_link_libraries(test_special_judge PRIVATE
    GTest::gtest_main
    mysql::concpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_special_judge PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)

# v1.3.1 — Admin Special Judge CRUD (header-only special_judge_repo
# + audit_log_repo, no httplib / nlohmann_json — the route layer's
# 256KB clamp is checked via a header grep in the test binary, not by
# instantiating the route). Same link set as test_special_judge.
target_link_libraries(test_admin_special_judge PRIVATE
    GTest::gtest_main
    mysql::concpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_admin_special_judge PRIVATE
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

# v1.2.68 Phase 9 ★ Prometheus 指标接入 — MetricsService +
# /api/v1/metrics 路由。metrics.h 是 header-only + inline；测试通过
# 进程内 httplib 客户端验证 /api/v1/metrics 的 Content-Type 与 body
# 形态，所以 link 集合与 test_health 一致（httplib + OpenSSL）。
target_link_libraries(test_metrics PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_metrics PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

add_test(NAME config            COMMAND test_config)
add_test(NAME config_singleton  COMMAND test_config_singleton)
add_test(NAME logger            COMMAND test_logger)
add_test(NAME connection_pool   COMMAND test_connection_pool)
add_test(NAME server            COMMAND test_server)
add_test(NAME uuid              COMMAND test_uuid)
add_test(NAME request_id        COMMAND test_request_id)
add_test(NAME health            COMMAND test_health)
add_test(NAME metrics           COMMAND test_metrics)
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
add_test(NAME special_judge      COMMAND test_special_judge)
add_test(NAME admin_special_judge COMMAND test_admin_special_judge)
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
# Phase 4 ☆ v1.2.18: special_judge_repo.h transitively brought into
# submission_routes.h pushes test_submission over the MSVC COFF section
# limit (C1128); /bigobj is the documented workaround (same trick used
# by test_admin_problem_crud / test_admin_bulk_import / main.cpp).
if(MSVC)
    target_compile_options(test_submission PRIVATE /bigobj)
endif()
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
# Phase 5 ★ v1.2.21: cookie_utils.h + auth_routes.h updates added
# inline Set-Cookie plumbing which — combined with submission_routes.h +
# judge_notifier.h — pushes test_submission_sse over MSVC's COFF
# section limit (C1128). /bigobj is the documented workaround.
# Same fix as test_submission / test_admin_problem_crud / test_admin_bulk_import.
if(MSVC)
    target_compile_options(test_submission_sse PRIVATE /bigobj)
endif()
add_test(NAME submission_sse      COMMAND test_submission_sse)

# cookie_utils.h + auth_routes.h Phase 5 ★ Token 存储: Set-Cookie
# issuance on login/register/refresh + Cookie reading on refresh +
# Max-Age=0 clear on logout. The unit tests cover the parser/builder
# with no MySQL; the integration tests drive the full HttpServer +
# ConnectionPool stack and assert the wire shape of every Set-Cookie
# header. Same link set as test_auth_refresh.
target_link_libraries(test_auth_cookie_storage PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_auth_cookie_storage PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
add_test(NAME auth_cookie_storage  COMMAND test_auth_cookie_storage)

# stats_routes.h (Phase 6 ★) — GET /api/v1/stats/profile/:username
# (SPEC §5.4, A15). End-to-end tests spin up the full HttpServer +
# ConnectionPool + RateLimiter + JWT stack, seed users / problems /
# submissions via raw SQL (avoiding cross-repo ODR collisions on
# user_repo.h::detail::req_string), and assert the envelope shape
# end-to-end. The pure unit tests (extract_username_from_path +
# serialize_by_status + serialize_by_difficulty + serialize_user_meta
# + serialize_user_stats + compute_user_stats) run without MySQL
# (the compute_user_stats test SKIPs when ping fails). Same link set
# as test_submission (the route handler uses require_authentication
# + reads via user_repo / submission_repo / problem_repo).
target_link_libraries(test_stats_profile PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_stats_profile PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
# stats_routes.h pulls in user_repo.h + submission_repo.h +
# problem_repo.h (Phase 2 + Phase 3 + Phase 4 repos), pushing the
# binary past MSVC's COFF section limit (C1128). /bigobj is the
# documented workaround (same fix as test_submission /
# test_admin_problem_crud / test_admin_bulk_import).
if(MSVC)
    target_compile_options(test_stats_profile PRIVATE /bigobj)
endif()
add_test(NAME stats_profile        COMMAND test_stats_profile)

# stats_routes.h (Phase 6 ★) — GET /api/v1/stats/ranking
# (SPEC §5.4, F11). Public endpoint with 30/min/IP rate limit.
# End-to-end tests spin up the full HttpServer + ConnectionPool +
# RateLimiter + JWT stack, seed users / problems / submissions via
# raw SQL (same ODR workaround as test_stats_profile), and assert
# the envelope shape end-to-end. The pure unit tests
# (parse_ranking_query + serialize_leaderboard_row + rank_for_offset
# + leaderboard constants) run without MySQL. Same link set as
# test_stats_profile (the route handler uses consume_rate_limit
# + reads via user_repo / submission_repo / problem_repo).
target_link_libraries(test_stats_ranking PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)
target_include_directories(test_stats_ranking PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
# Same /bigobj workaround as test_stats_profile: stats_routes.h
# pulls in user_repo.h + submission_repo.h + problem_repo.h.
if(MSVC)
    target_compile_options(test_stats_ranking PRIVATE /bigobj)
endif()
add_test(NAME stats_ranking        COMMAND test_stats_ranking)

# admin_user_routes.h (Phase 6 ★) — GET /api/v1/admin/users +
# PUT /api/v1/admin/users/:id/role (SPEC §5.5, A22, A24, A27).
# End-to-end tests spin up the full HttpServer + ConnectionPool +
# RateLimiter + JWT stack, seed users via raw SQL (same ODR
# workaround as test_stats_profile), insert audit_logs directly
# so the role-change audit-trail is exercised, and assert the
# envelope shape end-to-end. The pure unit tests
# (parse_role_param + parse_id_param + extract_id_from_path +
# parse_list_query + serialize_user_admin_row + constants) run
# without MySQL. Same link set as test_admin_problem_crud
# (the role-change path uses require_admin + writes to
# audit_logs + invokes user_repo + audit_log_repo).
target_link_libraries(test_admin_users PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_admin_users PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
# admin_user_routes.h transitively pulls in user_repo.h +
# audit_log_repo.h + problem_revisions_repo.h via the rate_limit
# header (revisions_repo is included indirectly through the
# admin namespace via submission_routes.h headers). MSVC /bigobj
# keeps us under the COFF section limit (same fix as
# test_admin_problem_crud / test_admin_bulk_import).
if(MSVC)
    target_compile_options(test_admin_users PRIVATE /bigobj)
endif()
add_test(NAME admin_users          COMMAND test_admin_users)

# admin_stats_routes.h (Phase 6 ★ v1.2.42) — GET /api/v1/admin/stats
# (SPEC §5.5). The pure unit tests cover the helper shape
# (all_status_keys / all_difficulty_keys / zero_padded_* /
# serialize_judge_subsystem / serialize_system_stats /
# snapshot_judge_subsystem). The integration tests cover the full
# HttpServer + ConnectionPool + JWT stack path with seeded data.
# The (c) tests additionally wire a real JudgeScheduler +
# WarmPool + docker_client (with ping-failing URL) to confirm the
# subsystem accessors flow through without crashing. Same link set
# as test_judge_scheduler (the live-subsystem test transitively
# needs docker_client + warm_pool + judge_scheduler headers).
target_link_libraries(test_admin_stats PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_admin_stats PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/judge)
# admin_stats_routes.h transitively pulls in user_repo.h +
# submission_repo.h + problem_repo.h + audit_log_repo.h + (in
# the (c) tests) judge_scheduler.h + warm_pool.h. Together this
# blows past MSVC's COFF section limit (C1128); /bigobj is the
# documented workaround (same fix as test_admin_users /
# test_admin_problem_crud / test_admin_bulk_import).
if(MSVC)
    target_compile_options(test_admin_stats PRIVATE /bigobj)
endif()
add_test(NAME admin_stats          COMMAND test_admin_stats)

# admin_audit_log_routes.h (Phase 6 ★ v1.2.43) — GET
# /api/v1/admin/audit-logs (SPEC §5.5, §15.6). Pulls in
# audit_log_repo.h + admin_middleware + rate_limit +
# error_handler. The pure unit tests (parse_admin_id_param +
# parse_list_query + serialize_audit_row + truncate_for_envelope)
# run without MySQL; the integration tests seed audit_logs via
# raw SQL (same ODR-safe workaround used by test_admin_users /
# test_stats_profile) so audit_log_repo.h::detail::req_string
# doesn't widen the ODR surface. Same link set as test_audit_log
# (mysql::concpp + nlohmann_json + jwt-cpp + httplib + bcrypt
# for the JWT signing test JWTs).
target_link_libraries(test_admin_audit_logs PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_admin_audit_logs PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
# admin_audit_log_routes.h pulls in audit_log_repo.h +
# rate_limit.h + error_handler.h. Combined with the in-process
# HttpServer + sqlitex_concpp + nlohmann_json headers, MSVC's
# COFF section limit (C1128) is hit; /bigobj is the documented
# workaround (same fix as test_admin_users / test_admin_stats /
# test_admin_problem_crud / test_admin_bulk_import).
if(MSVC)
    target_compile_options(test_admin_audit_logs PRIVATE /bigobj)
endif()
add_test(NAME admin_audit_logs     COMMAND test_admin_audit_logs)

# admin_queue_routes.h (Phase 6 ★ v1.2.44) — GET /api/v1/admin/queue
# (SPEC §5.5 row 4, §16.1). Pulls in admin_middleware + rate_limit +
# error_handler + system_routes. The pure unit tests (serialize_*,
# format_iso8601_utc, snapshot_queue_subsystem, kAdminQueueDefault)
# run without MySQL; the integration tests use a raw SQL seed for
# submissions + users (same ODR-safe workaround used by
# test_admin_audit_logs / test_admin_stats / test_admin_users).
# Same link set as test_admin_stats — the live-subsystem tests
# transitively need docker_client + warm_pool + judge_scheduler
# headers, and admin_queue_routes.h forward-declares them to avoid
# dragging docker_client + std::thread + std::filesystem into every
# TU. The (c) "subsystem" tests instantiate JudgeScheduler +
# WarmPool + Client against an unreachable docker socket, so
# gtest/gtest_main + httplib + nlohmann_json + jwt-cpp + mysql::concpp
# + bcrypt are all on the link line.
target_link_libraries(test_admin_queue PRIVATE
    GTest::gtest_main
    httplib::httplib
    nlohmann_json::nlohmann_json
    jwt-cpp::jwt-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)
target_include_directories(test_admin_queue PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/judge)
# admin_queue_routes.h + judge_scheduler.h + warm_pool.h +
# docker_client.h together blow past MSVC's COFF section limit
# (C1128) on the (c) subsystem tests; /bigobj is the documented
# workaround (same fix as test_admin_users / test_admin_stats /
# test_admin_audit_logs / test_admin_problem_crud /
# test_admin_bulk_import).
if(MSVC)
    target_compile_options(test_admin_queue PRIVATE /bigobj)
endif()
add_test(NAME admin_queue          COMMAND test_admin_queue)

# ───────────────────────────────────────────────────────────────────────────
#  v1.2.60 — Phase 8 ★ 单元测试覆盖率 ≥ 60%
#  新增 5 个 pure-unit test_*.cpp 覆盖 5 块核心模块(repo / audit / user /
#  problem / problem_revisions / submission / test_case)的 validators /
#  helpers / build_where_clause，过去只被 route-level 测试间接触达。
#  link 集合参考对应的 route-level test_* 二进制（mysql::concpp +
#  nlohmann_json + OpenSSL，部分 + jwt-cpp）。test_submission_repo 和
#  test_user_repo_helpers / test_problem_repo_helpers / test_audit_log_helpers
#  都是 pure C++（无 MySQL / httplib），只需要 conpp + json 的头文件路径。
# ───────────────────────────────────────────────────────────────────────────

# test_test_case_repo.cpp — sample_case 表的 6 个公共方法 +
# SampleCaseRow 默认值 + kSampleCaseSelectColumns 常量。IT 部分需要 MySQL，
# pure unit 部分纯 C++。Link 集合对齐 test_problem.cpp (mysql::concpp +
# OpenSSL)。
target_link_libraries(test_test_case_repo PRIVATE
    GTest::gtest_main
    mysql::concpp
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_test_case_repo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
add_test(NAME test_case_repo       COMMAND test_test_case_repo)

# test_submission_repo.cpp — submission_repo 的 pure unit validators
# (is_valid_status / is_terminal_status / is_valid_language /
# validate_code_length / clamp_list_filter / kStatus* / kMin/MaxCodeLength
# / kSubmissionSelectColumns + 异常层次 + struct defaults)。
# 全部纯 C++，无需 MySQL/httplib，但 include 路径走 ${CMAKE_SOURCE_DIR}/src
# 让 <db/submission_repo.h> 找得到。
target_link_libraries(test_submission_repo PRIVATE
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_submission_repo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
add_test(NAME submission_repo      COMMAND test_submission_repo)

# test_user_repo_helpers.cpp — user_repo 的 pure unit validators
# (is_valid_username_char / validate_username / validate_email /
# clamp_user_list_filter + 异常层次 + struct defaults)。纯 C++。
target_link_libraries(test_user_repo_helpers PRIVATE
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_user_repo_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
add_test(NAME user_repo_helpers    COMMAND test_user_repo_helpers)

# test_problem_repo_helpers.cpp — problem_repo + problem_revisions_repo 的
# pure unit validators + looks_like_dup_key_error heuristic + clamp_list_filter
# （两个 repo 的版本）。纯 C++，无 MySQL/httplib/jwt。
target_link_libraries(test_problem_repo_helpers PRIVATE
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_problem_repo_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
add_test(NAME problem_repo_helpers COMMAND test_problem_repo_helpers)

# test_audit_log_helpers.cpp — audit_log_repo 的 validators +
# detail::build_where_clause（覆盖 6 个 optional filter 的所有排列）。纯 C++，
# 但 build_where_clause 返回的 WhereClause 里 bound_str 是 std::string 数组，
# 所以不依赖 mysqlx；但 detail::WhereClause 在头文件里 #include 了
# <vector><string>，所以 link nlohmann_json + conpp 跟 test_audit_log 保持一致
# 避免某个间接 include 路径意外。
target_link_libraries(test_audit_log_helpers PRIVATE
    GTest::gtest_main
    mysql::concpp
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto)
target_include_directories(test_audit_log_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db)
add_test(NAME audit_log_helpers    COMMAND test_audit_log_helpers)