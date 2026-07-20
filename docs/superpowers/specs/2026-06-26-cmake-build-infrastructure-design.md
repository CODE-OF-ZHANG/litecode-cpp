# Phase 1 - 基础设施：项目目录结构 + CMake 构建 设计文档

> **日期**: 2026-06-26
> **范围**: SPEC.md TODO 清单 Phase 1 第一项——项目目录结构 + CMake 构建系统
> **状态**: 已确认

---

## 1. 决策摘要

| 决策点 | 选择 | 理由 |
|--------|------|------|
| CMake 架构 | 方案A：单目标 flat build | 所有模块为 header-only，单一 `main.cpp` 编译单元，MVP 足够简单 |
| bcrypt 策略 | 内嵌源码到 `src/auth/bcrypt/` | 跨平台零外部依赖，Windows 上 libbcrypt 编译问题多 |
| vcpkg | 不使用 | 安装慢/卡住，改用 FetchContent + 本地预编译包 |
| mysql-connector-c++ | 本地预编译包 | 已有 `D:\GitHub_CODE\mysql-connector-c++-9.7.0-winx64` |
| OpenSSL | 本地预编译包 | 下载到 `D:\GitHub_CODE\OpenSSL-Win64`，CMake HINTS 指向 |
| zlib | FetchContent (GitHub) | 和 header-only 库一样自动下载，无需手动安装 |

## 2. 依赖获取方式完整表

| 依赖 | 版本 | 获取方式 | CMake target / find_package |
|------|------|---------|---------------------------|
| cpp-httplib | v0.18.3 | FetchContent (GitHub) | `httplib::httplib` |
| jwt-cpp | v0.7.0 | FetchContent (GitHub) | `jwt-cpp::jwt-cpp` |
| nlohmann/json | v3.11.3 | FetchContent (GitHub) | `nlohmann_json::nlohmann_json` |
| zlib | 最新 | FetchContent (GitHub) | `ZLIB::ZLIB` |
| mysql-connector-c++ | 9.7.0 | 本地预编译 | `mysql::concpp` (via `find_package(mysql-concpp)`) |
| OpenSSL | 3.x | 本地预编译 | `OpenSSL::SSL`, `OpenSSL::Crypto` (via `find_package(OpenSSL)`) |
| bcrypt | — | 内嵌源码 | 编译 `bcrypt.c` + `blf.c`，通过 `password_hash.h` 封装 |

## 3. CMakeLists.txt 设计

```cmake
cmake_minimum_required(VERSION 3.15)
project(litecode LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Build output ───
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ─── Local dependency paths (Windows) ───
set(MYSQL_CONNECTOR_DIR "D:/GitHub_CODE/mysql-connector-c++-9.7.0-winx64"
    CACHE PATH "Path to mysql-connector-c++ installation")
set(OPENSSL_ROOT_DIR "D:/GitHub_CODE/OpenSSL-Win64"
    CACHE PATH "Path to OpenSSL installation")

# ─── FetchContent: header-only + zlib ───
include(FetchContent)

# cpp-httplib
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3)
FetchContent_MakeAvailable(httplib)

# jwt-cpp
FetchContent_Declare(jwt-cpp
    GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
    GIT_TAG        v0.7.0)
FetchContent_MakeAvailable(jwt-cpp)

# nlohmann/json
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3)
FetchContent_MakeAvailable(json)

# zlib
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1)
FetchContent_MakeAvailable(zlib)

# ─── find_package: local prebuilt ───
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
    OpenSSL::SSL OpenSSL::Crypto
    mysql::concpp)

# ─── Build type default ───
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
```

### 关键设计点

1. **LANGUAGES C CXX** — bcrypt 是 C 源码，需要启用 C 编译器
2. **CACHE PATH** — 本地依赖路径用 CACHE 变量，可通过 `-DMYSQL_CONNECTOR_DIR=...` 命令行覆盖
3. **zlib FetchContent** — 从 GitHub 自动拉取，与 header-only 库一致的处理方式
4. **mysql-concpp** — 用官方 CMake config 文件查找，target 名为 `mysql::connpp`

## 4. 目录结构调整

```
新增/填充的文件：
├── CMakeLists.txt              ← 填充完整内容
├── third_party/                ← 新增空目录（.gitkeep），FetchContent 备用
└── src/auth/bcrypt/            ← 填充 bcrypt 源码
    ├── bcrypt.h                ← C 头文件（extern "C" 包装）
    ├── bcrypt.c                ← bcrypt 核心实现
    ├── blf.h                   ← Blowfish cipher 头文件
    └── blf.c                   ← Blowfish cipher 实现
```

其余目录保持 SPEC.md Section 10 设计不变。

## 5. password_hash.h 接口

```cpp
// src/auth/password_hash.h
#pragma once
#include <string>

namespace litecode {

// 生成 bcrypt 哈希（自动生成 salt，默认 cost=12）
std::string password_hash(const std::string& password);

// 验证密码是否匹配哈希
bool password_verify(const std::string& password, const std::string& hash);

} // namespace litecode
```

内部调用 `src/auth/bcrypt/` 的 C 函数，对外暴露简洁的 C++ 接口。

## 6. 构建命令

```powershell
# 首次配置（Windows）
cmake -B build

# 构建
cmake --build build --config Release

# 运行
./build/bin/litecode
```

## 7. 依赖关系图

```
litecode (executable)
├── src/main.cpp              ← 唯一 C++ 编译单元
├── src/auth/bcrypt/*.c       ← C 源码，bcrypt 密码哈希
│
├── [FetchContent] cpp-httplib v0.18.3  → httplib::httplib
├── [FetchContent] jwt-cpp v0.7.0       → jwt-cpp::jwt-cpp
├── [FetchContent] nlohmann/json v3.11.3 → nlohmann_json::nlohmann_json
├── [FetchContent] zlib v1.3.1           → ZLIB::ZLIB
│
├── [本地] mysql-connector-c++ 9.7.0      → mysql::concpp
└── [本地] OpenSSL 3.x                   → OpenSSL::SSL, OpenSSL::Crypto
```