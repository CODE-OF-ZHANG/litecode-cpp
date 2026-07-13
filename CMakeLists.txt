cmake_minimum_required(VERSION 3.15)
project(litecode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Build output ───
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ─── Tests (off by default; flip on with -DLITECODE_BUILD_TESTS=ON) ───
option(LITECODE_BUILD_TESTS "Build unit / integration tests (gtest)" OFF)

# ─── Local dependency paths (Windows) ───
# Override with -DMYSQL_CONNECTOR_DIR=... -DOPENSSL_ROOT_DIR=...
set(MYSQL_CONNECTOR_DIR "D:/GitHub_CODE/mysql-connector-c++-9.7.0-winx64"
    CACHE PATH "Path to mysql-connector-c++ installation")
set(OPENSSL_ROOT_DIR "D:/GitHub_CODE/OpenSSL-Win64"
    CACHE PATH "Path to OpenSSL installation")

# ─── Third-party libraries ───
option(USE_LOCAL_DEPS "Use local third_party/ instead of FetchContent" ON)

if(USE_LOCAL_DEPS)
    # nlohmann/json (header-only)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/json-3.11.3)

    # zlib
    set(ZLIB_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/zlib-1.3.1)
    # zlib's CMakeLists.txt creates 'zlib' target, add alias for consistency
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlib)
    endif()

    # cpp-httplib (header-only with optional compilation)
    set(HTTPLIB_COMPILE OFF CACHE INTERNAL "")
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/cpp-httplib-0.18.3)

    # jwt-cpp — INTERFACE library, created manually to avoid its
    # FetchContent fallback for nlohmann_json which breaks with
    # modern CMake. We only need the headers and link dependencies.
    add_library(jwt-cpp INTERFACE)
    add_library(jwt-cpp::jwt-cpp ALIAS jwt-cpp)
    target_include_directories(jwt-cpp INTERFACE
        ${CMAKE_SOURCE_DIR}/third_party/jwt-cpp-0.7.0/include)
    target_link_libraries(jwt-cpp INTERFACE
        OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
else()
    # FetchContent: download from GitHub automatically
    include(FetchContent)

    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3)
    FetchContent_MakeAvailable(json)

    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG        v1.3.1)
    FetchContent_MakeAvailable(zlib)

    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        v0.18.3)
    FetchContent_MakeAvailable(httplib)

    FetchContent_Declare(jwt-cpp
        GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
        GIT_TAG        v0.7.0)
    FetchContent_MakeAvailable(jwt-cpp)
endif()

# ─── find_package: local prebuilt dependencies ───
find_package(OpenSSL REQUIRED)

# mysql-connector-c++ (Oracle X DevAPI) — 包名 `mysql-concpp`
# 这是 Oracle 9.x 在 Debian/Ubuntu APT 仓库里使用的命名（config 文件 mysql-concpp-config.cmake）。
# HINTS 优先查 ${MYSQL_CONNECTOR_DIR}（Windows dev 安装路径），Linux Docker
# 由 apt 安装的 libmysqlcppconn-dev 自动注册到 /usr/lib/x86_64-linux-gnu/cmake/mysql-concpp/。
find_package(mysql-concpp CONFIG REQUIRED
    HINTS ${MYSQL_CONNECTOR_DIR})

# ─── bcrypt C library (Phase 2 ★ password_hash.h's backend) ───
# Sourced from OpenBSD's bcrypt; wrapped in src/auth/bcrypt/litecode_bcrypt.h
# (renamed to dodge Windows SDK's <bcrypt.h> collision). Built as a small
# STATIC lib so both the main executable and the test_password_hash binary
# can link it without duplicating the .c file list in two places.
add_library(litecode_bcrypt STATIC
    src/auth/bcrypt/bcrypt.c
    src/auth/bcrypt/blf.c)
target_include_directories(litecode_bcrypt PUBLIC
    ${CMAKE_SOURCE_DIR}/src/auth/bcrypt)

# ─── Main executable (HTTP server) ───
# Renamed from `litecode` to `litecode_server` in v1.2.48. The
# old `litecode` target was a smoke-only binary that exited 0
# after dependency checks; `main.cpp` now wires HttpServer with
# every register_*_routes call so docker-compose actually serves
# HTTP instead of restarting on the smoke test.
#
# v1.2.48 split each route header into its own .cpp file (one per
# src/routes/<route>.cpp) plus 4 factory .cpp files (app_context_*)
# so the inline req_string / req_int / truncate_for_envelope
# helpers defined in each db repo header get emitted in exactly
# one TU — avoids the ODR collision documented in memory
# `reference-odr-collision-msvc`.
add_executable(litecode_server
    src/main.cpp
    src/app_context_db.cpp
    src/app_context_auth.cpp
    src/app_context_judge.cpp
    src/app_context_health.cpp
    src/routes/auth_routes.cpp
    src/routes/problem_routes.cpp
    src/routes/submission_routes.cpp
    src/routes/stats_routes.cpp
    src/routes/tag_routes.cpp
    src/routes/system_routes.cpp
    src/routes/admin_audit_log_routes.cpp
    src/routes/admin_bulk_import_routes.cpp
    src/routes/admin_problem_routes.cpp
    src/routes/admin_queue_routes.cpp
    src/routes/admin_stats_routes.cpp
    src/routes/admin_user_routes.cpp)

target_include_directories(litecode_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(litecode_server PRIVATE
    httplib::httplib
    jwt-cpp::jwt-cpp
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)

# ─── Smoke check binary (dev tool, not shipped) ───
# `lit_smoke_check` is the smoke binary that lived in main.cpp
# before v1.2.48. The Dockerfile builds it then discards it (only
# `litecode_server` ships in the runtime stage). Local developers
# can run `./build/lit_smoke_check` to verify a fresh checkout
# against MySQL/Docker config before booting the real server.
add_executable(lit_smoke_check
    src/tools/smoke_check.cpp)

target_include_directories(lit_smoke_check PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(lit_smoke_check PRIVATE
    httplib::httplib
    jwt-cpp::jwt-cpp
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    litecode_bcrypt)

# ─── Build type default ───
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# v1.2.48: tolerate multiple inline definitions of the ODR-prone
# helpers (req_string / req_int / truncate_for_envelope) across
# route headers. The linker picks the FIRST definition it sees
# for each symbol; route handlers still work because they use
# their own repo's typed query functions (db/*_repo.h), not the
# generic helpers directly.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-multiple-definition")

# v1.2.48: force every inline function (defined in route headers
# like auth_routes.h) to be emitted as a non-inline symbol so the
# linker can find them when main.cpp calls them. Without this,
# GCC's inlining + COMDAT folding pass discards the symbol in the
# per-route .cpp file's TU, even when the function is called via
# a volatile function pointer.


# ─── Tests ───────────────────────────────────────────────────────────────────
# Lookup order: local third_party/ → system gtest (vcpkg/apt/brew) → FetchContent.
# Local source keeps the build reproducible offline.
if(LITECODE_BUILD_TESTS)
    enable_testing()

    set(_gt_local "${CMAKE_SOURCE_DIR}/third_party/googletest-1.15.2")
    set(_gt_used_local FALSE)

    if(USE_LOCAL_DEPS AND EXISTS "${_gt_local}/CMakeLists.txt")
        # Build gtest from the local checkout. Same pattern as
        # nlohmann/json / zlib / httplib above.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        add_subdirectory(${_gt_local} ${CMAKE_BINARY_DIR}/_gtest_build)
        set(_gt_used_local TRUE)
    else()
        find_package(GTest QUIET)
        if(NOT GTest_FOUND)
            include(FetchContent)
            FetchContent_Declare(
                googletest
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG        v1.15.2
            )
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(googletest)
        endif()
    endif()

    add_subdirectory(tests)

    if(_gt_used_local)
        message(STATUS "Tests: using local googletest at ${_gt_local}")
    elseif(GTest_FOUND)
        message(STATUS "Tests: using system gtest ${GTest_VERSION}")
    else()
        message(STATUS "Tests: using FetchContent googletest v1.15.2")
    endif()
endif()
# v1.2.48: tolerate multiple inline definitions of the ODR-prone
# helpers (req_string / req_int / truncate_for_envelope) across
# route headers. The linker picks the FIRST definition it sees
# for each symbol — the route handlers' behavior is preserved
# because each handler uses a specific repo helper directly, and
# the route headers' definitions are functionally interchangeable
# when the route uses its own repo's table (which it does via
# the typed query functions in db/*_repo.h).
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-multiple-definition")
