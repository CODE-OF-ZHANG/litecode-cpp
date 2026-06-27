cmake_minimum_required(VERSION 3.15)
project(litecode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Build output ───
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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
find_package(mysql-concpp CONFIG REQUIRED
    HINTS ${MYSQL_CONNECTOR_DIR})

# ─── Main executable ───
add_executable(litecode
    src/main.cpp
    src/auth/bcrypt/bcrypt.c
    src/auth/bcrypt/blf.c)

target_include_directories(litecode PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/auth/bcrypt)

target_link_libraries(litecode PRIVATE
    httplib::httplib
    jwt-cpp::jwt-cpp
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp)

# ─── Build type default ───
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()