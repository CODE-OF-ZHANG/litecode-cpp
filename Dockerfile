# =============================================================
# LiteCode-CPP — Web 服务镜像（多阶段构建）
# =============================================================
# SPEC §11 Phase 1 / §15.5（Web 容器资源限制 + 非 root）
# -------------------------------------------------------------
# Stage 1 (builder): ubuntu:22.04 + g++ + Oracle MySQL Connector/C++ (X DevAPI)
# Stage 2 (runtime): ubuntu:22.04-slim + 仅运行时库 + litecode 二进制
#
# mysql-connector-c++ 9.x 通过 Oracle 官方 MySQL APT 仓库安装
# （Oracle 官方已不在 Downloads 页提供 Linux tarball，只有 .deb）
# =============================================================

# ───── Stage 1: builder ───────────────────────────────────────
FROM ubuntu:26.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 编译依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libssl-dev \
        zlib1g-dev \
        libcurl4-openssl-dev \
        uuid-dev \
        libprotobuf-dev \
        protobuf-compiler \
        ca-certificates \
        gnupg \
        git \
        curl \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# 添加 Oracle MySQL 官方 APT 仓库并安装 mysql-connector-c++ 9.x
# Oracle 仓库组件 mysql-tools 提供 libmysqlcppconn-dev / libmysqlcppconn10 / libmysqlcppconnx2
RUN curl -fsSL https://repo.mysql.com/RPM-GPG-KEY-mysql-2025 \
        | gpg --dearmor -o /usr/share/keyrings/mysql-apt.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/mysql-apt.gpg] https://repo.mysql.com/apt/ubuntu jammy mysql-tools" \
        > /etc/apt/sources.list.d/mysql.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        libmysqlcppconn-dev \
    && rm -rf /var/lib/apt/lists/* \
    # apt 包把头文件装在 /usr/include/mysql-cppconn/ 下（带命名空间前缀）
    # 源码使用 <mysqlx/xdevapi.h> 等无前缀路径，软链接桥接
    && cd /usr/include \
    && ln -sf mysql-cppconn/mysqlx mysqlx \
    && ln -sf mysql-cppconn/mysql  mysql  \
    && ln -sf mysql-cppconn/jdbc  jdbc  \
    && ls -la mysqlx mysql jdbc

WORKDIR /src

# 先拷贝 CMakeLists + third_party 头文件库，最大限度利用 Docker 缓存
COPY CMakeLists.txt ./
COPY third_party/ ./third_party/

# 拷贝源代码
COPY src/ ./src/

# 构建（Release 模式）
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DUSE_LOCAL_DEPS=ON \
    && cmake --build build -j2

# ───── Stage 2: runtime ──────────────────────────────────────
FROM ubuntu:26.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# 运行时依赖：
#   - libmysqlcppconn10: mysql-connector-c++ 运行时（核心）
#   - libmysqlcppconnx2: X DevAPI 扩展运行时
#   - libssl3 / zlib1g: jwt-cpp / cpp-httplib 依赖
#   - libcurl4 / libuuid1: 备用
#   - ca-certificates: TLS
#   - wget: 健康检查
#   - tini: PID 1 信号转发
#   - bash: 调试
#
# libmysqlcppconn* 来自 Oracle MySQL APT 仓库（jammy mysql-tools 组件）
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
        ca-certificates \
        gnupg \
    && curl -fsSL https://repo.mysql.com/RPM-GPG-KEY-mysql-2025 \
        | gpg --dearmor -o /usr/share/keyrings/mysql-apt.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/mysql-apt.gpg] https://repo.mysql.com/apt/ubuntu jammy mysql-tools" \
        > /etc/apt/sources.list.d/mysql.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        libmysqlcppconn10 \
        libmysqlcppconnx2 \
        libssl3 \
        zlib1g \
        libcurl4 \
        libuuid1 \
        ca-certificates \
        wget \
        tini \
        bash \
    && rm -rf /var/lib/apt/lists/*

# 拷贝编译产物
COPY --from=builder /src/build/bin/litecode_server /usr/local/bin/litecode_server

# 拷贝 Web 静态资源 + 题库
COPY web/ /app/web/

# 非 root 运行（SPEC §15.5）
# v1.3.4 PR 9 ★ 也建 /app/uploads(头像上传落点),named volume
# litecode-uploads 挂到这里,提前 chown 给 litecode 用户避免
# runtime Permission denied。
#
# v1.3.4 PR 13 ★ 也建 /tmp/litecode-judge(web→judge 任务目录共享
# 卷挂载点)。该路径在 /tmp 下,不在 chown -R /app 覆盖范围内;
# v1.2.50 引入此卷时 web 容器还跑 root,后切到 litecode(uid=1000)
# 只 chown 了 /app → /tmp/litecode-judge 保持 root:root 0755,
# create_directories 抛 Permission denied。Dockerfile 这一行负责
# 新部署直接生效(空 volume + 镜像目录所有权继承);entrypoint.sh
# 额外兜底自愈老部署的脏 volume。
RUN groupadd -g 1000 litecode \
    && useradd  -u 1000 -g litecode -m -s /bin/bash litecode \
    && mkdir -p /app/logs /app/uploads /tmp/litecode-judge \
    && chown -R litecode:litecode /app /tmp/litecode-judge

# v1.3.4 PR 13: entrypoint 兜底修复 named volume 所有权（详见
# docker-entrypoint.sh 注释）。在 tini 之前插入,确保 tini 仍是
# PID 1 接收信号。
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

USER litecode
WORKDIR /app

ENV SERVER_HOST=0.0.0.0 \
    SERVER_PORT=8080

EXPOSE 8080

# tini 负责信号转发，litecode 接收 SIGTERM 优雅退出
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/docker-entrypoint.sh"]
CMD ["/usr/local/bin/litecode_server"]

# 健康检查（SPEC §16.1）
HEALTHCHECK --interval=10s --timeout=5s --start-period=15s --retries=3 \
    CMD wget -qO- http://127.0.0.1:8080/api/v1/health || exit 1