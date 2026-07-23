#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — scripts/cleanup_demo_data.sh  (v1.3.4)
# -------------------------------------------------------------
# 清理 demo_submission.sh 跑出来的测试账号 + 级联删它们的
# submissions / problem_revisions / audit_logs。
#
# 触发场景:
#   - /ranking.html 上看见一堆 demo_user_<timestamp>_<rand> 这种
#     一眼就是测试脚本生成的"假排行"(solved=5 / 4 / 2 / 1 ...) —
#     它们污染了真实用户的排行榜视觉,清掉后榜单只剩真人。
#   - 运维需要给真实用户演示干净排名时随手清理。
#
# 关键设计:
#   * WHERE 限定 username LIKE 'demo_user_%' — 仅命中脚本随机生成
#     的 demo_user 账号,不会误删普通用户。
#   * ON DELETE CASCADE:submissions.user_id + audit_logs.admin_id
#     + problem_revisions.editor_id 三处外键已有 CASCADE / SET NULL
#     处理(users.id 删除时自动级联清理)。
#   * 默认是 DRY_RUN:只 SELECT 待删 + 关联 cascade 计数,不实际
#     执行 DELETE。带 --yes 才真删。
#   * 删完再 SELECT 一次验证结果。
#
# 用法:
#   bash scripts/cleanup_demo_data.sh               # 默认 dry-run
#   bash scripts/cleanup_demo_data.sh --yes         # 真删
#   CONTAINER=litecode-mysql bash scripts/cleanup_demo_data.sh --yes
#
# 退出码:
#   0  —— 成功(无论 dry-run 或真删)
#   1  —— 容器不可达 / SQL 异常
# =============================================================
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"

CONTAINER="${CONTAINER:-litecode-mysql}"
MYSQL_PWD="${MYSQL_PWD:-123456}"
DB="${DB:-litecode}"

# ─────────── args ───────────
DO_IT=0
for arg in "$@"; do
    case "${arg}" in
        --yes|-y) DO_IT=1 ;;
        --help|-h)
            sed -n '2,/^# ====/p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "unknown arg: ${arg}" >&2; exit 2 ;;
    esac
done

# ─────────── sanity ───────────
if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker 不在 PATH" >&2
    exit 1
fi
if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "${CONTAINER}"; then
    echo "ERROR: 容器 ${CONTAINER} 不在运行" >&2
    echo "       (用  CONTAINER=<name> bash $0 --yes  指定其他名字)" >&2
    exit 1
fi

run_sql() {
    docker exec -i -e MYSQL_PWD="${MYSQL_PWD}" "${CONTAINER}" \
        mysql -uroot --protocol=TCP "${DB}" "$@"
}

# ─────────── dry-run 报告 ───────────
echo "[1/3] 预览:待清理 demo_user + cascade 数据统计"
COUNT_QUERY="
SELECT
    (SELECT COUNT(*) FROM users WHERE username LIKE 'demo_user_%')              AS demo_users,
    (SELECT COUNT(*) FROM submissions WHERE user_id IN
        (SELECT id FROM users WHERE username LIKE 'demo_user_%'))             AS cascade_subs,
    (SELECT COUNT(*) FROM audit_logs WHERE admin_id IN
        (SELECT id FROM users WHERE username LIKE 'demo_user_%'))             AS cascade_audits,
    (SELECT COUNT(*) FROM problem_revisions WHERE editor_id IN
        (SELECT id FROM users WHERE username LIKE 'demo_user_%'))             AS cascade_revs;
"

if ! STATS="$(run_sql -N -e "${COUNT_QUERY}" 2>&1)"; then
    echo "ERROR: SELECT 失败 — ${STATS}" >&2
    exit 1
fi

DEMO_USERS="$(echo "${STATS}" | awk '{print $1}')"
CASCADE_SUBS="$(echo "${STATS}" | awk '{print $2}')"
CASCADE_AUDS="$(echo "${STATS}" | awk '{print $3}')"
CASCADE_REVS="$(echo "${STATS}" | awk '{print $4}')"

echo "  待删 demo_user 数:        ${DEMO_USERS}"
echo "  cascade 删除 submissions: ${CASCADE_SUBS}"
echo "  cascade 改写 audit_logs:  ${CASCADE_AUDS}  (admin_id → NULL)"
echo "  cascade 改写 revisions:   ${CASCADE_REVS}  (editor_id → NULL)"
echo ""

if [ "${DEMO_USERS}" -eq 0 ]; then
    echo "[ok] DB 里没有 demo_user — 无需清理"
    exit 0
fi

if [ "${DO_IT}" -eq 0 ]; then
    echo "[dry-run] 没传 --yes,跳过 DELETE。重新跑带 --yes 真删:"
    echo "         bash scripts/cleanup_demo_data.sh --yes"
    exit 0
fi

# ─────────── 真删 ───────────
echo "[2/3] 执行 DELETE (transactional)"
DELETE_SQL="
START TRANSACTION;
DELETE FROM users WHERE username LIKE 'demo_user_%';
COMMIT;
"
if ! run_sql -e "${DELETE_SQL}" 2>&1; then
    echo "ERROR: DELETE 失败" >&2
    run_sql -e "ROLLBACK;" >/dev/null 2>&1
    exit 1
fi

# ─────────── 验证 ───────────
echo "[3/3] 验证:DB 里已无 demo_user"
VERIFY="$(run_sql -N -e "SELECT COUNT(*) FROM users WHERE username LIKE 'demo_user_%';" 2>&1)"
if [ "${VERIFY}" != "0" ]; then
    echo "ERROR: 删后仍有 ${VERIFY} 个 demo_user 残留 — 回滚检查" >&2
    exit 1
fi

LEFT_USERS="$(run_sql -N -e "SELECT COUNT(*) FROM users;" 2>&1)"
LEFT_SUBS="$(run_sql -N -e "SELECT COUNT(*) FROM submissions;" 2>&1)"
echo "  users 剩余:        ${LEFT_USERS}"
echo "  submissions 剩余:  ${LEFT_SUBS}"
echo ""
echo "[ok] 清理完成。/ranking.html 现在只显示真实用户。"
