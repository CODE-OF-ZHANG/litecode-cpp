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
# =============================================================
set -euo pipefail

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
command -v mysql      >/dev/null 2>&1 || { echo "[✗] mysql client not found" >&2; exit 1; }
command -v mysqldump  >/dev/null 2>&1 || { echo "[✗] mysqldump not found"  >&2; exit 1; }

if [ -z "$MYSQL_PASSWORD" ]; then
    echo "[✗] MYSQL_PASSWORD / MYSQL_ROOT_PASSWORD 未设置" >&2
    exit 1
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
    gunzip -t "$OUTFILE" || { echo "[✗] gzip 完整性校验失败" >&2; exit 1; }
fi

echo "[✓] 备份完成：${OUTFILE}（$(numfmt --to=iec --suffix=B "$SIZE" 2>/dev/null || echo "${SIZE}B")）"

# ───── 3. 清理过期 ─────────────────────────────────────
if [ -d "$BACKUP_DIR" ]; then
    DELETED=$(find "$BACKUP_DIR" -maxdepth 1 -name "litecode_${DB_NAME}_*.sql*" \
              -mtime "+${BACKUP_RETENTION_DAYS}" -print -delete | wc -l)
    echo "[*] 清理 ${BACKUP_RETENTION_DAYS} 天前的备份：删除 ${DELETED} 个"
fi

# ───── 4. 异地同步（可选）─────────────────────────────
if [ -n "$RCLONE_REMOTE" ]; then
    if command -v rclone >/dev/null 2>&1; then
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
