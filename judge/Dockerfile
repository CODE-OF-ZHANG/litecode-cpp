# =============================================================
# LiteCode-CPP — 判题镜像
# =============================================================
# SPEC §7.2 / §7.3 / §15.4
#   - §7.2  Ubuntu + g++/gcc + gdb + coreutils + dos2unix + judge.sh
#   - §7.3  非 root (USER judgeuser) + WORKDIR /judge + ENTRYPOINT judge.sh
#   - §15.4 编译安全标志 + 编译独立超时 + 运行硬超时 + OLE + 文本归一化
#
# 资源硬限制由 docker run 注入 (--cpus / --memory / --pids-limit /
# --network=none / --read-only / --security-opt no-new-privileges),
# 见 SPEC §7.3 / §15.5 表格 + SPEC §3.2 判题流程图。
#
# 此镜像在 docker-compose 里被 judge 服务构建一次（构建完退出），
# web 服务通过 docker run 临时启动判题容器复用此镜像：
#   docker run --rm \
#     --network=none --read-only \
#     --cpus=1 --memory=256m --pids-limit=50 \
#     --security-opt no-new-privileges \
#     --tmpfs /tmp:size=64m,mode=1777 \
#     -v <task.json>:/judge/task.json:ro \
#     litecode-judge:latest /judge/task.json
# =============================================================

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# 仅装判题所需依赖：
#   - g++/gcc/gdb         编译 C/C++ 提交
#   - coreutils           timeout / tr / head / tail / cmp / tee
#   - dos2unix            Windows 换行兜底
#   - jq                  解析任务 JSON（SPEC §7.1 的输入契约）
#   - libc6-dev           <stdio.h>/<stdlib.h> 等运行时头文件
#   - ca-certificates     升级证书（防御供应链）
#   - gawk                浮点 epsilon 比对（compare.sh:66-124 用多维数组
#                         a_tok[lnA][i]；busybox mawk 不支持多维数组）。
#                         ubuntu:22.04 默认 /usr/bin/awk 指向 mawk，仅装包
#                         不会改 update-alternatives 默认 → 显式 set。
# --
# SPEC §7.4 CE/RE 截断用 head/tail；浮点比对用 awk（必须 gawk）；其它 GNU
# 用户态工具已通过 coreutils 提供，不再多装包以缩小镜像与攻击面。
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        gcc \
        gdb \
        coreutils \
        dos2unix \
        jq \
        libc6-dev \
        ca-certificates \
        gawk \
    && update-alternatives --set awk /usr/bin/gawk \
    && rm -rf /var/lib/apt/lists/* \
    && find /usr/share/doc /usr/share/man -type f -delete 2>/dev/null || true

# judge.sh（判题入口；容器启动即执行）+ lib/*.sh 子模块
COPY judge.sh /usr/local/bin/judge.sh
COPY lib/     /usr/local/lib/judge/
RUN chmod +x /usr/local/bin/judge.sh \
    && chmod +x /usr/local/lib/judge/*.sh

# 非 root 运行（SPEC §7.3 / §15.4）
# 即使容器被逃逸，也没有 root 权限可提。
# 1000 是 SPEC §7.2 规定的 UID，与 web 服务镜像的 litecode 用户错开，
# 避免两镜像共享宿主机 /etc/passwd 时冲突。
RUN useradd -m -u 1000 -s /bin/bash -d /judge judgeuser \
    && mkdir -p /judge /tmp/judge \
    && chown -R judgeuser:judgeuser /judge /tmp/judge

USER judgeuser
WORKDIR /judge

# 资源硬限制由 docker run 命令注入，不可在本镜像固定。

ENTRYPOINT ["/usr/local/bin/judge.sh"]
CMD ["--help"]
