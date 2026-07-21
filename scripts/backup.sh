#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — 数据库备份脚本（SPEC §16.5 / Phase 7 ☆）
# =============================================================
# 每日 mysqldump 全量 + 压缩 + 保留 N 天 + 可选异地同步。
#
# 两种运行方式：
#   (a) 容器内（推荐）—— 由 docker-compose 的 backup 服务以 cron
#       方式调用。镜像基于 mysql:8.0 客户端，避免重复装 mysql-client。
#       docker compose exec backup /usr/local/bin/backup.sh
#
#   (b) 宿主机直接跑 —— 适用于没启 backup 服务、或 OS-level cron 备份。
#       前提：宿主机装了 mysql-client（或 mycli）。
#       MYSQL_HOST=mysql ./scripts/backup.sh
#
# 异地备份（可选）：
#   - 装 rclone 后，`rclone config` 加一个 remote（如 s3 / b2 / webdav）
#   - export RCLONE_REMOTE="myremote:bucket/litecode-backups"
#   - 脚本在本地写完后自动 rclone copy 到该 remote
#
# 灾备恢复（README/deployment.md 详述）：
#   gunzip -c litecode_YYYY-MM-DD_HHMMSS.sql.gz \
#     | docker exec -i litecode-mysql mysql -uroot -p"$ROOTPASS" litecode
#
# 自检 / CI 集成（v1.2.75）：
#   - `scripts/lint.sh backup`  静态环节点 grep
#   - `scripts/e2e_acceptance.sh` A46  委托运行 + 反向汇入
#   - BACKUP_STRICT=1 把「缺前置」从 info 升级为 FAIL（CI 强约束）
#   - 末行输出 `BACKUP_RESULT PASS=N FAIL=N SKIP=N`（与 v1.2.67 FUZZ_RESULT /
#     v1.2.72 DRILL_RESULT / v1.2.74 PROFILE_RESULT 风格一致）
#   - 能力探测：第二段开头输出 `capabilities: mysql=... mysqldump=... gzip=...
#     zstd=... rclone=... backup_dir=...` 让 lint 能 grep
# =============================================================
set -euo pipefail

# ───── 自检开关（v1.2.75）────────────────────────────
BACKUP_STRICT="${BACKUP_STRICT:-0}"     # 0 = 缺前置 info（默认）；1 = 缺前置 exit 1
BACKUP_DRY_RUN="${BACKUP_DRY_RUN:-0}"   # 1 = 只探测不真正 dump（CI / e2e 用）
PASS=0; FAIL=0; SKIP=0
emit_result() {
    # 单次任务只有 1 个断言（dump + gunzip + size），所以计数是 0/1
    echo "BACKUP_RESULT PASS=${PASS} FAIL=${FAIL} SKIP=${SKIP}"
}
trap 'emit_result' EXIT

# ───── 默认值（可被环境变量覆盖）──────────────────────────
MYSQL_HOST="${MYSQL_HOST:-mysql}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-${MYSQL_ROOT_PASSWORD:-}}"
DB_NAME="${DB_NAME:-litecode}"

BACKUP_DIR="${BACKUP_DIR:-/backup}"            # docker volume 挂载点 / 宿主目录
BACKUP_RETENTION_DAYS="${BACKUP_RETENTION_DAYS:-14}"
BACKUP_COMPRESS="${BACKUP_COMPRESS:-gzip}"     # gzip | zstd | none

# 异地同步（可选；不设则跳过）
RCLONE_REMOTE="${RCLONE_REMOTE:-}"
RCLONE_BWLIMIT="${RCLONE_BWLIMIT:-0}"          # 0 = 不限速；生产可设 10M

# ───── 工具检查 ─────────────────────────────────────────
HAVE_MYSQL=0;      command -v mysql      >/dev/null 2>&1 && HAVE_MYSQL=1
HAVE_MYSQLDUMP=0;  command -v mysqldump  >/dev/null 2>&1 && HAVE_MYSQLDUMP=1
HAVE_GZIP=0;       command -v gzip       >/dev/null 2>&1 && HAVE_GZIP=1
HAVE_ZSTD=0;       command -v zstd       >/dev/null 2>&1 && HAVE_ZSTD=1
HAVE_RCLONE=0;     command -v rclone     >/dev/null 2>&1 && HAVE_RCLONE=1

# 能力探测（v1.2.75：让 lint / e2e 能 grep 确认）
cap() { [ "$1" = "1" ] && echo "ok" || echo "missing"; }
echo "capabilities: mysql=$(cap $HAVE_MYSQL) mysqldump=$(cap $HAVE_MYSQLDUMP) gzip=$(cap $HAVE_GZIP) zstd=$(cap $HAVE_ZSTD) rclone=$(cap $HAVE_RCLONE) backup_dir=${BACKUP_DIR} strict=${BACKUP_STRICT} dry_run=${BACKUP_DRY_RUN}"

# BACKUP_DRY_RUN 提前退出（不真正 dump；给 CI / e2e 静态探测用）
if [ "$BACKUP_DRY_RUN" = "1" ]; then
    echo "[*] BACKUP_DRY_RUN=1，跳过 mysqldump / 校验 / rclone"
    SKIP=1
    exit 0
fi

if [ "$HAVE_MYSQL" != "1" ]; then
    if [ "$BACKUP_STRICT" = "1" ]; then
        echo "[✗] mysql client not found（BACKUP_STRICT=1 升级为 fail）" >&2; exit 1
    fi
    echo "[!] mysql client not found（BACKUP_STRICT=0 跳过；装 mysql-client 后再跑）" >&2; SKIP=1; exit 0
fi
if [ "$HAVE_MYSQLDUMP" != "1" ]; then
    if [ "$BACKUP_STRICT" = "1" ]; then
        echo "[✗] mysqldump not found（BACKUP_STRICT=1 升级为 fail）" >&2; exit 1
    fi
    echo "[!] mysqldump not found（BACKUP_STRICT=0 跳过）" >&2; SKIP=1; exit 0
fi

if [ -z "$MYSQL_PASSWORD" ]; then
    if [ "$BACKUP_STRICT" = "1" ]; then
        echo "[✗] MYSQL_PASSWORD / MYSQL_ROOT_PASSWORD 未设置（STRICT 升级 fail）" >&2; exit 1
    fi
    echo "[!] MYSQL_PASSWORD / MYSQL_ROOT_PASSWORD 未设置（BACKUP_STRICT=0 跳过）" >&2; SKIP=1; exit 0
fi

# ───── 路径 / 文件名 ────────────────────────────────────
mkdir -p "$BACKUP_DIR"
STAMP="$(date +%Y-%m-%d_%H%M%S)"
BASENAME="litecode_${DB_NAME}_${STAMP}"
EXT=".sql"
case "$BACKUP_COMPRESS" in
    gzip) EXT=".sql.gz" ;;
    zstd) EXT=".sql.zst" ;;
    none) EXT=".sql" ;;
esac
OUTFILE="${BACKUP_DIR}/${BASENAME}${EXT}"

# ───── 1. mysqldump ──────────────────────────────────────
echo "[*] $(date -Iseconds) 开始备份 ${DB_NAME}@${MYSQL_HOST}:${MYSQL_PORT}"
echo "    → ${OUTFILE}"

DUMP_ARGS=(
    --host="$MYSQL_HOST"
    --port="$MYSQL_PORT"
    --user="$MYSQL_USER"
    --password="$MYSQL_PASSWORD"
    --single-transaction        # 事务一致性快照（InnoDB 必备）
    --quick                     # 大表逐行 dump，不缓存到内存
    --routines                  # 含 stored procs / functions
    --triggers                  # 含 triggers（默认其实包含，显式声明）
    --events                    # 含 scheduled events
    --set-gtid-purged=OFF       # 备份恢复目标未必启 GTID；OFF 让脚本可移植
    --default-character-set=utf8mb4
    --hex-blob                  # BLOB 列 hex 编码（防 charset 损坏）
    --no-tablespaces            # 不锁 tablespace 权限（共享实例安全）
    "$DB_NAME"
)

case "$BACKUP_COMPRESS" in
    gzip)
        mysqldump "${DUMP_ARGS[@]}" | gzip -9 > "$OUTFILE" ;;
    zstd)
        mysqldump "${DUMP_ARGS[@]}" | zstd -q -19 -T0 > "$OUTFILE" ;;
    none)
        mysqldump "${DUMP_ARGS[@]}" > "$OUTFILE" ;;
    *)
        echo "[✗] BACKUP_COMPRESS=$BACKUP_COMPRESS 不支持（gzip|zstd|none）" >&2
        exit 1 ;;
esac

# ───── 2. 校验 ──────────────────────────────────────────
SIZE=$(stat -c%s "$OUTFILE" 2>/dev/null || stat -f%z "$OUTFILE")
if [ "${SIZE:-0}" -lt 1024 ]; then
    echo "[✗] 备份文件过小（${SIZE} bytes），疑似失败" >&2
    rm -f "$OUTFILE"
    exit 1
fi

# gzip 完整性校验
if [ "$BACKUP_COMPRESS" = "gzip" ]; then
    if ! gunzip -t "$OUTFILE"; then
        echo "[✗] gzip 完整性校验失败" >&2
        rm -f "$OUTFILE"
        FAIL=1
        exit 1
    fi
fi

echo "[✓] 备份完成：${OUTFILE}（$(numfmt --to=iec --suffix=B "$SIZE" 2>/dev/null || echo "${SIZE}B")）"
PASS=1

# ───── 3. 清理过期 ─────────────────────────────────────
if [ -d "$BACKUP_DIR" ]; then
    DELETED=$(find "$BACKUP_DIR" -maxdepth 1 -name "litecode_${DB_NAME}_*.sql*" \
              -mtime "+${BACKUP_RETENTION_DAYS}" -print -delete | wc -l)
    echo "[*] 清理 ${BACKUP_RETENTION_DAYS} 天前的备份：删除 ${DELETED} 个"
fi

# ───── 4. 异地同步（可选）─────────────────────────────
if [ -n "$RCLONE_REMOTE" ]; then
    if [ "$HAVE_RCLONE" = "1" ]; then
        echo "[*] 异地同步 → ${RCLONE_REMOTE}"
        # 单文件 copy + bwlimit；--no-traverse 加速
        rclone copyto "$OUTFILE" "${RCLONE_REMOTE}/$(basename "$OUTFILE")" \
            --bwlimit "${RCLONE_BWLIMIT}" \
            --stats 30s --stats-one-line \
            --log-level INFO --retries 3 --low-level-retries 5
        echo "[✓] 异地同步完成"
    else
        echo "[!] RCLONE_REMOTE 已设置但 rclone 未安装，跳过异地同步" >&2
    fi
fi

echo "[✓] $(date -Iseconds) 备份任务结束"
