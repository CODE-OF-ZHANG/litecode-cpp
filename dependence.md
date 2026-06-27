# LiteCode-CPP — 依赖清单与安装指南

> **版本**: v1.1 MVP  
> **日期**: 2026-06-25  
> **适用平台**: Windows / Linux / macOS

---

## 1. 开发环境基础依赖

| 依赖 | 版本要求 | 用途 | 必需? |
|------|----------|------|-------|
| CMake | ≥ 3.15 | 构建系统 | ✅ |
| g++ / gcc | 支持 C++17 | C++ 编译器 | ✅ |
| Docker Desktop | 最新稳定版 | 判题沙箱 + 部署编排 | ✅ |
| Docker Compose | v2（随 Docker Desktop 安装） | 一键启动多容器 | ✅ |
| MySQL | 8.0 | 关系型数据库 | ✅（推荐 Docker 运行） |
| Python | ≥ 3.8 | 种子数据脚本（可选） | 可选 |

> **说明**：MySQL 和判题容器均通过 Docker 运行，本地开发无需单独安装 MySQL。

---

## 2. C++ 第三方库依赖

### 2.1 Header-Only 库（CMake FetchContent 自动下载）

以下库无需手动安装，CMake 构建时自动从 GitHub 拉取：

| 库 | 版本 | 用途 | 仓库 |
|----|------|------|------|
| cpp-httplib | v0.18.3 | HTTP 服务器框架 | https://github.com/yhirose/cpp-httplib |
| jwt-cpp | v0.7.0 | JWT 令牌生成与验证 | https://github.com/Thalhammer/jwt-cpp |
| nlohmann/json | v3.11.3 | JSON 解析与序列化 | https://github.com/nlohmann/json |

#### CMakeLists.txt FetchContent 配置

```cmake
include(FetchContent)

# cpp-httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
)
FetchContent_MakeAvailable(httplib)

# jwt-cpp
FetchContent_Declare(
    jwt-cpp
    GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
    GIT_TAG        v0.7.0
)
FetchContent_MakeAvailable(jwt-cpp)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)
```

### 2.2 需编译安装的库

| 库 | 用途 | 必需? |
|----|------|-------|
| mysql-connector-c++ | MySQL C++ 驱动，连接池与数据库操作 | ✅ |
| libbcrypt | 密码 bcrypt 哈希 | ✅ |
| zlib | cpp-httplib 压缩支持（可选但推荐） | 推荐 |

---

## 3. 前端依赖（CDN 引入）

无需本地安装，在 HTML 中通过 CDN 直接引用：

| 依赖 | 用途 | CDN 示例 |
|------|------|----------|
| CodeMirror 6 | 代码编辑器（语法高亮、自动补全） | `https://cdn.jsdelivr.net/npm/codemirror@6/...` |
| 或 Monaco Editor | 代码编辑器（VS Code 内核） | `https://cdn.jsdelivr.net/npm/monaco-editor@0.45/...` |

> MVP 阶段选择 **CodeMirror 6** 或 **Monaco Editor** 其中之一即可，CDN 方式引入，无需 npm 构建。

---

## 4. 判题 Docker 镜像依赖

判题镜像在 `judge/Dockerfile` 中定义，构建时自动安装：

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    g++ gcc gdb \
    && rm -rf /var/lib/apt/lists/*
COPY judge.sh /usr/local/bin/judge.sh
RUN chmod +x /usr/local/bin/judge.sh
WORKDIR /judge
ENTRYPOINT ["/usr/local/bin/judge.sh"]
```

包含的软件：

| 软件 | 用途 |
|------|------|
| g++ | 编译用户提交的 C++ 代码 |
| gcc | 编译用户提交的 C 代码 |
| gdb | 调试支持（可选） |

---

## 5. Python 依赖（种子数据脚本）

`scripts/seed_problems.py` 所需依赖：

```bash
pip install pyyaml mysql-connector-python
```

| 包 | 用途 | 必需? |
|----|------|-------|
| pyyaml | 解析 YAML 格式题目数据 | 可选（仅种子脚本） |
| mysql-connector-python | Python 连接 MySQL 导入数据 | 可选（仅种子脚本） |

---

## 6. 一键安装指南

### 6.1 Windows 环境

```powershell
# ============================================
# LiteCode-CPP Windows 开发环境一键搭建
# ============================================

# 1. 安装 Docker Desktop（含 Docker Compose v2）
winget install Docker.DockerDesktop

# 2. 安装 CMake
winget install Kitware.CMake

# 3. 安装 MSYS2（提供 g++ C++17 编译器）
winget install msys2.msys2
# 安装完成后，在 MSYS2 终端中执行：
#   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make
# 并将 C:\msys64\mingw64\bin 加入系统 PATH

# 4. 安装 vcpkg（管理 C++ 编译型依赖）
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install mysql-connector-cpp:x64-windows libbcrypt:x64-windows zlib:x64-windows

# 5. 配置 CMake 集成 vcpkg（后续构建时使用）
# cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# 6. Python 依赖（可选，用于种子数据脚本）
pip install pyyaml mysql-connector-python
```

### 6.2 Linux / WSL 环境

```bash
#!/bin/bash
# ============================================
# LiteCode-CPP Linux 开发环境一键搭建
# ============================================

# 1. 安装编译工具链和系统库
sudo apt-get update && sudo apt-get install -y \
    build-essential \
    cmake \
    g++ \
    gdb \
    libmysqlcppconn-dev \
    libssl-dev \
    libz-dev \
    python3 \
    python3-pip

# 2. 安装 Docker
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
# 重新登录终端使 docker 组生效

# 3. 安装 Docker Compose（如未随 Docker 安装）
sudo apt-get install -y docker-compose-plugin

# 4. 安装 libbcrypt（如系统仓库无，则从源码编译）
if ! dpkg -l | grep -q libbcrypt; then
    git clone https://github.com/rg3/bcrypt.git /tmp/libbcrypt
    cd /tmp/libbcrypt
    make
    sudo make install
    cd -
    rm -rf /tmp/libbcrypt
fi

# 5. Python 依赖（可选，用于种子数据脚本）
pip3 install pyyaml mysql-connector-python

# 6. Header-only 库无需手动安装
#    cpp-httplib / jwt-cpp / nlohmann/json 由 CMake FetchContent 自动下载
echo "Header-only 库将在 cmake --build 时自动下载"
```

### 6.3 macOS 环境

```bash
#!/bin/bash
# ============================================
# LiteCode-CPP macOS 开发环境一键搭建
# ============================================

# 1. 安装 Homebrew（如未安装）
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 2. 安装编译工具链
brew install cmake gcc

# 3. 安装 Docker Desktop
brew install --cask docker

# 4. 安装 C++ 依赖
brew install mysql-connector-c++ zlib openssl

# 5. 安装 libbcrypt（从源码）
git clone https://github.com/rg3/bcrypt.git /tmp/libbcrypt
cd /tmp/libbcrypt && make && sudo make install && cd -

# 6. Python 依赖（可选）
pip3 install pyyaml mysql-connector-python
```

---

## 7. 依赖版本锁定清单

| 分类 | 依赖项 | 版本 | 安装方式 | 必需? |
|------|--------|------|----------|-------|
| 编译 | CMake | ≥ 3.15 | 包管理器 | ✅ |
| 编译 | g++ / gcc | C++17 支持 | 包管理器 | ✅ |
| 运行 | Docker | 最新稳定版 | 包管理器 | ✅ |
| 运行 | Docker Compose | v2 | 随 Docker 安装 | ✅ |
| 运行 | MySQL | 8.0 | Docker 容器 | ✅ |
| C++ 库 | cpp-httplib | v0.18.3 | FetchContent | ✅ |
| C++ 库 | jwt-cpp | v0.7.0 | FetchContent | ✅ |
| C++ 库 | nlohmann/json | v3.11.3 | FetchContent | ✅ |
| C++ 库 | mysql-connector-c++ | 最新稳定版 | vcpkg / apt | ✅ |
| C++ 库 | libbcrypt | 最新稳定版 | vcpkg / 源码 | ✅ |
| C++ 库 | zlib | 最新稳定版 | vcpkg / apt | 推荐 |
| 前端 | CodeMirror 6 | 最新版 | CDN | ✅ |
| 脚本 | Python | ≥ 3.8 | 包管理器 | 可选 |
| 脚本 | PyYAML | 最新版 | pip | 可选 |
| 脚本 | mysql-connector-python | 最新版 | pip | 可选 |

---

## 8. 常见问题

### Q1: FetchContent 下载慢或失败？

设置代理或镜像：

```cmake
# 在 CMakeLists.txt 中设置代理
set(HTTP_PROXY "http://127.0.0.1:7890")
set(HTTPS_PROXY "http://127.0.0.1:7890")
```

或手动下载 header-only 库放到 `third_party/` 目录，修改 FetchContent 为本地路径。

### Q2: Windows 下 vcpkg 安装 mysql-connector-c++ 失败？

确保已安装 Visual Studio Build Tools（含 C++ 桌面开发工作负载），或尝试使用 MinGW 工具链：

```powershell
vcpkg install mysql-connector-cpp:x64-mingw-dynamic
```

### Q3: libbcrypt 在 Windows 上编译报错？

可考虑使用替代方案：
- 使用 OpenSSL 的 `PKCS5_PBKDF2_HMAC` 实现密码哈希
- 或直接在项目中集成 `bcrypt.c` 源码（来自 libbcrypt 仓库）

### Q4: Docker Desktop 启动后 WSL 集成问题？

在 Docker Desktop 设置中启用：**Settings → Resources → WSL Integration → 启用对应发行版**。

---

*本文档与 SPEC.md 配套使用，依赖版本以 SPEC.md 第 9 节技术栈为准。*