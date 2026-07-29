#!/bin/bash
# =============================================================
# LiteCode-CPP — Web 容器 entrypoint
# =============================================================
# v1.3.4 PR 13:
# 兜底修复 named volume 在 /tmp 等 chown-R /app 覆盖不到的路径上的
# 权限遗留 —— v1.2.50 引入 judge-tmp volume 时 web 容器还跑 root,
# 后来切到 litecode(uid=1000)只 chown 了 /app。/tmp/litecode-judge
# 的挂载点保持 root:root 0755,导致 std::filesystem::create_directories
# 抛 Permission denied → "运行样例失败 internal error"。
#
# entrypoint 在 tini→litecode_server 启动前以 root 身份跑(USER 还没切
# 到 litecode),把卷路径 chown 给 litecode 用户后 exec 真正的 CMD。
# 即使 volume 已经被前面 root 容器写过也照样自愈,比让运维手动
# `docker volume rm litecode-judge-tmp` 友好。
# =============================================================
set -e

# /tmp/litecode-judge: web→judge 任务目录共享卷 (v1.2.50)
# /app/uploads:       用户头像等上传资源 (v1.3.4 PR 9, /app 下本应
#                     已被 chown,但 volume 已是空时挂载也再 chown 一遍
#                     兜底,跨版本部署更鲁棒)
for d in /tmp/litecode-judge /app/uploads; do
    if [ -d "$d" ]; then
        chown -R 1000:1000 "$d" 2>/dev/null || true
    fi
done

exec "$@"
