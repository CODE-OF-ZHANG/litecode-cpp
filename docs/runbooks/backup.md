# 数据库备份 Runbook

> **SPEC**：§16.5 备份与恢复 / Phase 7 ☆ 备份脚本
> **自动化**：
>   - 脚本：`scripts/backup.sh`（v1.2.57 容器侧 dcron + v1.2.75 宿主机侧加固自检）
>   - 自检：`scripts/lint.sh backup`（bash -n + 22 项关键环节点 + 行数 sanity）
>   - 端到端：`scripts/e2e_acceptance.sh` A46（静态配置 + runbook 引用 + DRY_RUN 委托反向汇入）
>   - 验证：每月 1 次 [`docs/runbooks/monthly-restore-drill.md`](monthly-restore-drill.md) 灌库演练（v1.2.72）
> **节奏**：
>   - 生产：docker-compose `backup` 服务 dcron `0 3 * * *` 每日凌晨 03:00 自动跑
>   - 宿主机 cron（如未启 backup 服务）：`0 3 * * * /path/to/scripts/backup.sh >> /var/log/litecode-backup.log 2>&1`

---

## 1. 目的

把 SPEC 「mysqldump 每日 + 异地」**端到端落地**——不是「在 SPEC 里写一条就算数」，
而是「真能跑出 .sql.gz / 真能异地同步 / 真能 3-2-1 副本 / 真能入 CI / 真能入月度告警」。

备份三件套的关系：

- **本脚本（v1.2.75 强化）** —— *生产*（dump + 压缩 + 14 天本地保留 + rclone 异地）
- **[`monthly-restore-drill.md`](monthly-restore-drill.md)（v1.2.72）** —— *验证*（每月 1 次把最近一份 backup 灌回隔离栈跑 smoke）
- **`docs/runbooks/alerting.md`（v1.2.73）** —— *告警*（备份任务失败 / 备份文件 < 1MB / 24h 内无新 backup → 通知）

三者并列，不互替。

---

## 2. 落地清单（v1.2.75）

| 组件 | 文件 | 作用 |
|------|------|------|
| 主脚本 | `scripts/backup.sh` | mysqldump + gzip/zstd + 14 天保留 + rclone 异地 |
| 容器服务 | `docker-compose.yaml` `backup` service | alpine + dcron `0 3 * * *` 自动跑 |
| 自检 | `scripts/lint.sh backup` | bash -n + 22 项关键环节点 + 行数 sanity |
| e2e | `scripts/e2e_acceptance.sh` A46 | 静态配置 + runbook 引用 + DRY_RUN 委托反向汇入 |
| 验证 | `scripts/restore_drill.sh` | 月度灌库演练（独立 runbook） |
| 本 runbook | `docs/runbooks/backup.md` | 本文 |

---

## 3. SPEC §16.5 备份策略

| 项 | 标准 | 落地方式 |
|----|------|----------|
| 备份频率 | mysqldump 每日 1 次 | dcron `0 3 * * *` |
| 备份方式 | 全量 + `single-transaction` 一致性快照 | mysqldump `--single-transaction --quick --routines --triggers --events --hex-blob --set-gtid-purged=OFF` |
| 字符集 | utf8mb4 | `--default-character-set=utf8mb4` |
| 压缩 | gzip -9 / zstd -19 二选一 | `BACKUP_COMPRESS=gzip\|zstd\|none` |
| 本地保留 | 14 天 | `BACKUP_RETENTION_DAYS=14` |
| 异地同步 | 3-2-1（本地 1 + 异地 1 + 异地不同介质 1） | `RCLONE_REMOTE` 注入 → rclone copyto 到 S3 / OSS / B2 / WebDAV |
| 完整性校验 | dump 后立即 `gunzip -t` | 失败 → 删文件 + exit 1 |
| 容量校验 | 备份文件 < 1KB → 判失败 | `stat -c%s` 检查 |
| 告警 | 备份失败 / 文件过小 / 24h 无新文件 → 通知 | v1.2.73 alerting runbook |
| 验证 | 每月 1 次 restore drill 灌库 | `scripts/restore_drill.sh` |

阈值常量在 `scripts/backup.sh` 头部 `BACKUP_*` env vars；改默认值不需要改脚本逻辑。

---

## 4. 两种运行方式

### 4.1 容器内（推荐生产）

docker-compose 11 个服务里的 `backup`：

- **镜像**：`alpine:3.19` + `dcron` + `mysql-client` + `rclone`
- **卷挂载**：
  - `/backup`（命名卷 `litecode-backup-data`，跨容器重启保留）
  - `/etc/litecode/backup.env`（ro，注入 `MYSQL_*` / `RCLONE_*` env）
- **网络**：`litecode-net`（与 mysql 同网络，`MYSQL_HOST=mysql`）
- **调度**：dcron `0 3 * * *` 每日 03:00 自动跑 + 把 stdout/stderr 落 `/var/log/litecode/backup.log`
- **手动触发**：
  ```bash
  docker compose exec backup /usr/local/bin/backup.sh
  docker compose --profile backup up backup
  ```

### 4.2 宿主机直跑（适用于没起 backup 服务 / OS-level cron / K8s CronJob）

```bash
# 前置：装 mysql-client + 可选 rclone
sudo apt-get install -y mysql-client         # Debian/Ubuntu
brew install mysql-client rclone             # macOS（仅本地开发）

# 跑
export MYSQL_HOST=mysql
export MYSQL_PORT=3306
export MYSQL_USER=root
export MYSQL_PASSWORD='<from .env>'
export BACKUP_DIR=/var/backups/litecode      # 默认 /backup（容器内路径）
export RCLONE_REMOTE="b2:my-bucket/litecode" # 不设则跳过异地
bash scripts/backup.sh
```

---

## 5. 还原操作

### 5.1 灌回 MySQL（标准路径）

```bash
# 1. 找最新备份
ls -lh /backup/litecode_litecode_*.sql.gz | tail -1

# 2. 完整性自检
gunzip -t /backup/litecode_litecode_2026-07-15_030000.sql.gz \
    && echo "[✓] gzip 完整"

# 3. 灌回（灌前先 stop web / judge 避免写冲突）
docker compose stop web judge
gunzip -c /backup/litecode_litecode_2026-07-15_030000.sql.gz \
    | docker exec -i litecode-mysql \
        mysql -uroot -p"$MYSQL_ROOT_PASSWORD" litecode
docker compose start web judge
```

### 5.2 时间点恢复（PITR，litecode 暂未启用 binlog）

**当前版本只能恢复到「最近一次备份」的时间点**——如果两次备份之间丢了数据，
那部分数据无法恢复。要做 PITR 需要：

1. MySQL 启 binlog（`log_bin = /var/log/mysql/mysql-bin.log`）
2. 备份脚本额外打 `FLUSH BINARY LOGS` 锁当前 binlog
3. 恢复时先灌最近全量 + replay binlog

属于 v1.3+ 规划；当前 14 天保留 + 月度演练对中小型 OJ 足够。

### 5.3 异地恢复（灾难场景）

```bash
# 1. 从异地拉最近一份
rclone copyto b2:my-bucket/litecode/litecode_litecode_2026-07-15_030000.sql.gz \
    /tmp/recovery.sql.gz

# 2. 同 5.1 灌回（异地可能慢，按 ms 级带宽可能要 30 分钟）
```

---

## 6. 3-2-1 备份原则

| 副本 | 存储位置 | 介质 | 落地 |
|------|---------|------|------|
| 第 1 份 | 本机 `/backup` 命名卷 | 同机房 SSD | docker-compose `backup` 服务 |
| 第 2 份 | 异地 object storage（S3 / OSS / B2） | 公有云 | `RCLONE_REMOTE=b2:my-bucket/litecode` |
| 第 3 份 | 异地冷存储（Glacier / Archive） | 同上不同介质 | `RCLONE_REMOTE_COLD`（v1.3+，见 §10） |

任一副本丢失不影响恢复能力；本地机房整个挂掉也能从异地拉回。

---

## 7. 一键操作

### 7.1 跑自检 / e2e

```bash
# 静态（无栈依赖）
bash scripts/lint.sh backup

# 端到端（含 live 栈）
bash scripts/e2e_acceptance.sh   # 默认宽松
E2E_STRICT=1 BACKUP_STRICT=1 bash scripts/e2e_acceptance.sh  # CI 强约束
```

### 7.2 手动触发一次

```bash
# 容器侧
docker compose exec backup /usr/local/bin/backup.sh

# 宿主机侧
bash scripts/backup.sh

# DRY_RUN（不真 dump，仅能力探测）
BACKUP_DRY_RUN=1 bash scripts/backup.sh
```

### 7.3 看上一次结果

```bash
# 容器侧
docker compose logs --tail=50 backup

# 宿主机侧
tail -50 /var/log/litecode/backup.log

# 看 30 天内所有备份大小
ls -lh /backup/litecode_litecode_*.sql.gz
```

### 7.4 集成到发版门禁 / 月度告警

CI workflow（v1.2.58）当前跑 `lint.sh backup` + `e2e A46a/b`，不跑 `A46c`（避免 e2e 拖慢）。
**月度演练**走单独 cron（v1.2.72）：

```yaml
# .github/workflows/monthly-restore-drill.yml（v1.2.72 落地）
on:
  schedule:
    - cron: '0 9 1-7 * 1'   # 每月第 1 个周一 09:00
```

---

## 8. 故障排查

### 8.1 备份文件 < 1KB 立即被删

`scripts/backup.sh` 第 96-102 行：备份 < 1KB 视作失败，自动 `rm` + exit 1。

最常见 root cause：

- **`MYSQL_PASSWORD` 错** → mysqldump 早就 fail 但 `set -euo pipefail` + pipe 没传 exit code。看 stderr
- **MySQL 没启 / 网络隔离** → 容器内 `MYSQL_HOST=mysql`；若 mysql 容器名变了（重命名服务）脚本要同步
- **磁盘满** → 写 .sql.gz 到一半 ENOSPC；`df -h /backup` 查空间

### 8.2 `gunzip -t` 完整性校验失败

```bash
# 手动重测
gunzip -t /backup/litecode_litecode_2026-07-15_030000.sql.gz

# 看是不是磁盘 bit rot
sha256sum /backup/litecode_litecode_2026-07-15_030000.sql.gz
#   对比 rclone 上传的异地副本 sha256
rclone hashsum sha256 b2:my-bucket/litecode/litecode_litecode_2026-07-15_030000.sql.gz
```

如果本地坏、异地好 = 本地盘问题（写 `HostDiskSpaceLow` alert 即时感知）。
如果两边都坏 = 上传前已坏（dump 阶段就坏了）→ 看 §8.1。

### 8.3 rclone 异地同步失败

```bash
# 测连通性
rclone lsd b2:my-bucket/        # 列目录
rclone about b2:my-bucket/      # 看 quota

# 重试配置
rclone config reconnect b2:
```

如果 `RCLONE_REMOTE` 已设但 rclone 未装，脚本会 [!] 警告而非 fail（容器镜像自带 rclone）。
宿主机跑需要 `apt install rclone` 或 `brew install rclone`。

### 8.4 备份任务挂起 / 永远跑不完

```bash
# 看进程
ps aux | grep -E 'mysqldump|gzip|zstd'

# 看 MySQL 那边
docker exec litecode-mysql mysql -e 'SHOW PROCESSLIST'

# 大库 dump 卡在 --single-transaction 的常见原因：
#   - 长事务未提交（SHOW ENGINE INNODB STATUS）
#   - 网络拥塞（dump → gzip 管道缓）
#   - binlog 撑爆磁盘（SHOW BINARY LOGS）
```

### 8.5 磁盘空间涨满 / 14 天保留失效

```bash
# 立即手动清理
find /backup -maxdepth 1 -name 'litecode_litecode_*.sql*' -mtime +14 -print -delete

# 看是谁占空间
du -sh /backup/*
df -h /backup
```

如果 `BACKUP_RETENTION_DAYS=14` 还爆，说明日增量 > 总容量 1/14，**要么加容量要么缩保留天数**。

---

## 9. 与 restore_drill / alerting / deployment 的关系

| 工件 | 回答的问题 | 文件 |
|------|-----------|------|
| **`backup.sh`（本文 v1.2.75）** | 「backup 真能产出 + 真能异地同步吗？」 | `/backup/litecode_litecode_*.sql.gz` + rclone |
| [`restore_drill.sh`（v1.2.72）](../../scripts/restore_drill.sh) | 「backup 灌回去能跑起来吗？」 | [`monthly-restore-drill.md`](monthly-restore-drill.md) |
| [`prometheus-alerts.yml`（v1.2.73）](../../monitoring/alerting/) | 「backup 失败 / 文件过小 / 24h 无新文件要通知吗？」 | [`alerting.md`](alerting.md) |
| [`docs/deployment.md §5.4`](../../deployment.md) | 「灾备恢复 / 初始管理员 / 故障排查在 README 哪里？」 | `docs/deployment.md` |

灾备问题的标准诊断流程：

```
1. Prometheus 触发 BackupFailed / BackupFileTooSmall / BackupStale 告警
   ↓
2. 查 docs/runbooks/backup.md §8 故障排查定位 root cause
   ↓
3. 修完后手动跑一次 backup.sh 验证
   ↓
4. 触发 restore_drill.sh 灌回验证
   ↓
5. 升级版本（如需要）+ 更新 runbook
```

---

## 10. 已知未落地 / follow-up

### 10.1 异地冷备（Glacier / Archive）

当前只同步到 `RCLONE_REMOTE` 一个远端（§6 第 2 份）。第 3 份冷备需要
rclone `--backend s3` + 单独 bucket + lifecycle rule 转 Glacier。

属于 v1.3+；当前 S3 Standard 已经能扛一般机房故障。

### 10.2 PITR（point-in-time recovery）

需要 MySQL 启 binlog + 备份脚本 `FLUSH BINARY LOGS` + 恢复时 replay binlog。
§5.2 已说明；属于 v1.3+。

### 10.3 加密备份

当前异地备份是**明文 .sql.gz**。如果远端被攻破则数据库泄漏。
两种解决：

1. 备份脚本额外 `gpg --symmetric --cipher-algo AES256` → 落 `.sql.gz.gpg`
2. 远端 bucket 启 SSE-KMS（rclone `--sse-kms-key-id`）

属于 v1.3+。

### 10.4 备份文件元数据审计

每份备份打 `{size, sha256, dump_wall_ms, mysql_version, schema_version}` 落 sidecar `.meta.json`，月度演练时强校验「这份 backup 是当时完整 DB 的真实快照」。属于 v1.3+。

---

## 11. 版本演进

| 版本 | 改动 |
|------|------|
| v1.2.57 | docker-compose `backup` 服务（alpine + dcron + gzip/zstd + 14 天保留 + rclone） |
| v1.2.72 | `restore_drill.sh` 月度演练验证 backup 可灌回（独立 runbook） |
| v1.2.73 | alerting：`BackupFailed` / `BackupFileTooSmall` / `BackupStale` 告警规则 + runbook |
| **v1.2.75** | **本文** + `scripts/backup.sh` 自检/e2e/反向汇入加固（`BACKUP_RESULT` + `BACKUP_STRICT` + `BACKUP_DRY_RUN` + capabilities 探测）+ `scripts/lint.sh backup` 22 项关键环节点 + e2e A46 三层 + SPEC.md sync 翻 10 条 |
| v1.3+ (planned) | PITR / 加密 / 冷备 / sidecar 元数据 |