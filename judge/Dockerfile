# =============================================================
# LiteCode-CPP — 判题镜像
# =============================================================
# SPEC §7.2 / §7.3 / §15.4：g++ 编译环境 + judge.sh + 非 root
# -------------------------------------------------------------
# - 仅装 g++/gcc/gdb + coreutils/dos2unix，不装额外包
# - USER judgeuser：即使容器逃逸也无 root
# - WORKDIR /judge：编译/运行产物都落在这里
# - ENTRYPOINT judge.sh：容器启动即执行判题脚本
# =============================================================

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# 仅装必要的编译器与文本处理工具
#   - g++ / gcc: 编译 C/C++ 提交
#   - gdb: 调试信息（保留以便后续集成）
#   - coreutils: timeout / tr / head / tail 等基础命令
#   - dos2unix: 处理 Windows 换行
#   - libc6-dev: 头文件 + 链接器
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        gcc \
        gdb \
        coreutils \
        dos2unix \
        libc6-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 判题执行脚本（Phase 4 才会填充完整逻辑，这里先占位以便构建通过）
COPY judge.sh /usr/local/bin/judge.sh
RUN chmod +x /usr/local/bin/judge.sh

# 非 root 运行（SPEC §15.4）
RUN useradd -m -u 1000 -s /bin/bash judgeuser \
    && mkdir -p /judge /tmp/judge \
    && chown -R judgeuser:judgeuser /judge /tmp/judge

USER judgeuser
WORKDIR /judge

# 资源硬限制由 Docker run 命令注入（cpus/memory/pids-limit/read-only 等），
# 见 SPEC §7.3 / §15.5 表格。

ENTRYPOINT ["/usr/local/bin/judge.sh"]
CMD ["--help"]