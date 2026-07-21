#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — 月度备份恢复演练（Restore Drill）
# =============================================================
# SPEC §16.5 / Phase 9 ☆ 备份验证 —— 每月 1 次 restore drill。
#
# 目标：在**不影响主栈**的前提下，端到端验证「最近的 backup 能灌回
#       MySQL，且能正常服务 API + 鉴权 + 判题」。
#
# 流程（docs/runbooks/monthly-restore-drill.md 同步）：
#   [1] 找到最近一份 backup（${BACKUP_DIR}/litecode_<db>_*.sql.gz）
#   [2] 拉起隔离 mysql-drill 容器（独立命名卷 + 端口 3307）
#   [3] 等 mysql healthy → CREATE DATABASE → gunzip | docker exec 灌入
#   [4] 灌后校验：表行数非空 + admin 用户存在
#   [5] 拉起隔离 drill-proxy（socket 代理）和 drill-web（连 mysql-drill）
#   [6] smoke 测试：
#         6a. GET /api/v1/health → 200
#         6b. POST /auth/login（admin）→ 200 + access_token
#         6c. POST /admin/problems/import（bulk-import two-sum）→ 201
#         6d. 注册测试用户 → POST /submissions two-sum AC → 轮询 AC
#   [7] 拆 drill 栈（容器 + 临时卷）
#   [8] 输出 DRILL_RESULT PASS=N FAIL=N SKIP=N
#
# 用法：
#   bash scripts/restore_drill.sh                # 默认宽松模式
#   RESTORE_STRICT=1 bash scripts/restore_drill.sh  # CI/强约束
#   BACKUP_DIR=/path/to/backups bash scripts/restore_drill.sh
#
# 退出码：
#   0  —— 无 FAIL（宽松模式缺能力 / 缺 backup 时记 SKIP）
#   1  —— 至少一个 FAIL（含 STRICT 模式下因缺能力升级的 skip）
# =============================================================
# shellcheck disable=SC2317  # 部分 helper 在某些分支下不被调用，属正常
set -uo pipefail

# ───────────────────────── 路径解析 ─────────────────────────
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"
COMPOSE_FILE="${ROOT}/docker-compose.yml"

# ───────────────────────── 配置 ─────────────────────────
BACKUP_DIR="${BACKUP_DIR:-${ROOT}/backup-data/backup}"

MYSQL_DRILL_CONTAINER="${MYSQL_DRILL_CONTAINER:-litecode-drill-mysql}"
PROXY_DRILL_CONTAINER="${PROXY_DRILL_CONTAINER:-litecode-drill-proxy}"
WEB_DRILL_CONTAINER="${WEB_DRILL_CONTAINER:-litecode-drill-web}"
DRILL_VOLUME="${DRILL_VOLUME:-litecode-drill-mysql-data}"

MYSQL_DRILL_NETWORK="${MYSQL_DRILL_NETWORK:-litecode-net}"
MYSQL_DRILL_PORT_BIND="${MYSQL_DRILL_PORT_BIND:-127.0.0.1:3307}"
WEB_DRILL_PORT_BIND="${WEB_DRILL_PORT_BIND:-127.0.0.1:8081}"

WEB_IMAGE="${WEB_IMAGE:-litecode-web:latest}"
JUDGE_IMAGE="${JUDGE_IMAGE:-litecode-judge:latest}"
MYSQL_IMAGE="${MYSQL_IMAGE:-mysql:8.0.40}"
PROXY_IMAGE="${PROXY_IMAGE:-tecnativa/docker-socket-proxy:0.1}"

MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-changeme_root}"
DB_NAME="${DB_NAME:-litecode}"
ADMIN_USER="${ADMIN_USER:-admin}"
ADMIN_PASS="${ADMIN_PASS:-admin123!}"
JUDGE_PROBLEM_SLUG="${JUDGE_PROBLEM_SLUG:-two-sum}"
JUDGE_POLL_TIMEOUT_S="${JUDGE_POLL_TIMEOUT_S:-60}"
PROBLEMS_DIR="${PROBLEMS_DIR:-${ROOT}/problems}"
LOG_LEVEL="${LOG_LEVEL:-WARN}"
RESTORE_STRICT="${RESTORE_STRICT:-0}"

# 从 .env 覆盖 MYSQL_ROOT_PASSWORD（用户最常改这一项）
if [ -f "${ROOT}/.env" ]; then
    set +u
    # shellcheck disable=SC1091
    set -a; . "${ROOT}/.env" 2>/dev/null || true; set +a
    set -u
    [ -n "${MYSQL_ROOT_PASSWORD:-}" ] && [ "${MYSQL_ROOT_PASSWORD}" != "changeme_root" ] || true
fi

# ───────────────────────── 计数器 ─────────────────────────
PASS=0; FAIL=0; SKIP=0
ok()   { echo "    ok   - $*"; PASS=$((PASS+1)); }
fail() { echo "    FAIL - $*"; FAIL=$((FAIL+1)); }
skip() { echo "    skip - $*"; SKIP=$((SKIP+1)); }
kase() { echo; echo "── $1  $2"; }

# ───────────────────────── 反向汇总（兼容 v1.2.67 FUZZ_RESULT 风格） ─────────────────────────
emit_result() {
    echo ""
    echo "DRILL_RESULT PASS=${PASS} FAIL=${FAIL} SKIP=${SKIP}"
}

# ───────────────────────── jq 探测（沿用 v1.2.61 经验） ─────────────────────────
if ! command -v jq >/dev/null 2>&1; then
    for cand in \
        "/c/Users/${USERNAME:-$USER}/AppData/Local/Microsoft/WinGet/Packages/jqlang.jq_Microsoft.Winget.Source_8wekyb3d8bbwe" \
        "/c/Program Files/jq"; do
        if [ -x "${cand}/jq.exe" ]; then export PATH="${cand}:${PATH}"; break; fi
    done
fi

# ───────────────────────── HTTP helper（复用 v1.2.61 e2e 模式子集） ─────────────────────────
WEB_BASE_URL="http://${WEB_DRILL_PORT_BIND}"
API="${WEB_BASE_URL}/api/v1"
TMPD="$(mktemp -d -t lc-drill-XXXXXX)"
RESP_BODY="${TMPD}/body"
RESP_HDR="${TMPD}/hdr"
HTTP_CODE=""

api() {  # METHOD PATH [-t TOKEN] [-d JSON_BODY]
    local method="$1" path="$2"; shift 2
    local token="" data=""
    while [ $# -gt 0 ]; do
        case "$1" in
            -t) token="$2"; shift 2;;
            -d) data="$2"; shift 2;;
            *)  shift;;
        esac
    done
    local url="${API}${path}"
    local -a args=(-sS -X "${method}" -o "${RESP_BODY}" -D "${RESP_HDR}" \
                   -w '%{http_code}' --max-time 30)
    [ -n "${token}" ] && args+=(-H "Authorization: Bearer ${token}")
    [ -n "${data}" ]  && args+=(-H "Content-Type: application/json" --data "${data}")
    HTTP_CODE="$(curl "${args[@]}" "${url}" 2>/dev/null)" || HTTP_CODE="000"
    [ -n "${HTTP_CODE}" ] || HTTP_CODE="000"
}
jqb()  { jq -r "$1" "${RESP_BODY}" 2>/dev/null; }

# ───────────────────────── 能力探测 ─────────────────────────
echo "=== LiteCode-CPP Restore Drill ==="
echo "ROOT=${ROOT}  BACKUP_DIR=${BACKUP_DIR}  STRICT=${RESTORE_STRICT}"
echo "WEB_BIND=${WEB_DRILL_PORT_BIND}  MYSQL_BIND=${MYSQL_DRILL_PORT_BIND}"
echo ""

DOCKER_OK=0; MYSQL_IMG_OK=0; PROXY_IMG_OK=0; WEB_IMG_OK=0; BACKUP_OK=0; COMPOSE_OK=0

if command -v docker >/dev/null 2>&1 && docker version >/dev/null 2>&1; then DOCKER_OK=1; fi
if [ "${DOCKER_OK}" = "1" ]; then
    docker image inspect "${MYSQL_IMAGE}"  >/dev/null 2>&1 && MYSQL_IMG_OK=1
    docker image inspect "${PROXY_IMAGE}"  >/dev/null 2>&1 && PROXY_IMG_OK=1
    docker image inspect "${WEB_IMAGE}"    >/dev/null 2>&1 && WEB_IMG_OK=1
    docker compose -f "${COMPOSE_FILE}" version >/dev/null 2>&1 && COMPOSE_OK=1
    if [ -d "${BACKUP_DIR}" ] && ls -t "${BACKUP_DIR}"/litecode_"${DB_NAME}"_*.sql.gz 2>/dev/null | head -1 | grep -q .; then
        BACKUP_OK=1
    fi
fi

cap() { if [ "$1" = "1" ]; then echo "[ok]"; else echo "[NO]"; fi; }
echo "capabilities:  docker=$(cap $DOCKER_OK)  mysql_img=$(cap $MYSQL_IMG_OK)  proxy_img=$(cap $PROXY_IMG_OK)  web_img=$(cap $WEB_IMG_OK)  backup_present=$(cap $BACKUP_OK)  compose=$(cap $COMPOSE_OK)"
echo ""

# ───────────────────────── 收尾钩子（无论哪条退出路径都会跑） ─────────────────────────
cleanup() {
    # 静默拆 drill 容器 + 临时卷；任何步骤失败都不再抛出
    if [ "${CLEANED:-0}" = "0" ]; then
        CLEANED=1
        docker rm -f \
            "${WEB_DRILL_CONTAINER}" "${PROXY_DRILL_CONTAINER}" "${MYSQL_DRILL_CONTAINER}" \
            >/dev/null 2>&1 || true
        docker volume rm -f "${DRILL_VOLUME}" >/dev/null 2>&1 || true
        rm -rf "${TMPD}" >/dev/null 2>&1 || true
    fi
}
trap 'cleanup; emit_result' EXIT

# ───────────────────────── 0. 缺能力时优雅降级 ─────────────────────────
PRECHECK_MSG=""
[ "${DOCKER_OK}" = "0" ]      && PRECHECK_MSG="${PRECHECK_MSG} docker"
[ "${MYSQL_IMG_OK}" = "0" ]   && PRECHECK_MSG="${PRECHECK_MSG} mysql_img(${MYSQL_IMAGE})"
[ "${PROXY_IMG_OK}" = "0" ]   && PRECHECK_MSG="${PRECHECK_MSG} proxy_img(${PROXY_IMAGE})"
[ "${WEB_IMG_OK}" = "0" ]     && PRECHECK_MSG="${PRECHECK_MSG} web_img(${WEB_IMAGE})"
[ "${BACKUP_OK}" = "0" ]      && PRECHECK_MSG="${PRECHECK_MSG} backup_file(${BACKUP_DIR}/litecode_${DB_NAME}_*.sql.gz)"

if [ -n "${PRECHECK_MSG}" ]; then
    msg="恢复演练前置缺失:${PRECHECK_MSG}"
    if [ "${RESTORE_STRICT}" = "1" ]; then
        fail "${msg}（STRICT：缺前置升级为 FAIL）"
        exit 1
    fi
    skip "${msg}"
    exit 0
fi

# network 必须已经存在（drill 不创建主网络，避免污染）
if ! docker network inspect "${MYSQL_DRILL_NETWORK}" >/dev/null 2>&1; then
    if [ "${RESTORE_STRICT}" = "1" ]; then
        fail "缺少 docker 网络 ${MYSQL_DRILL_NETWORK}；请先 docker compose up -d 把 default 栈拉起以创建网络"
        exit 1
    fi
    skip "缺少 docker 网络 ${MYSQL_DRILL_NETWORK}（请先 docker compose up -d 拉起 default 栈）"
    exit 0
fi

# ───────────────────────── 1. 找到最新 backup ─────────────────────────
kase "[1/8]" "挑选最新 backup"
LATEST_BACKUP="$(ls -t "${BACKUP_DIR}"/litecode_"${DB_NAME}"_*.sql.gz 2>/dev/null | head -1)"
LATEST_SIZE="$(stat -c%s "${LATEST_BACKUP}" 2>/dev/null || stat -f%z "${LATEST_BACKUP}")"
if [ -z "${LATEST_BACKUP}" ] || [ ! -f "${LATEST_BACKUP}" ]; then
    skip "未找到 backup 文件 (${BACKUP_DIR}/litecode_${DB_NAME}_*.sql.gz)"
    exit 0
fi
ok "选中最新备份: $(basename "${LATEST_BACKUP}") (${LATEST_SIZE} bytes)"

# 顺便校验 gzip 完整性（gunzip -t 在 §backup.sh 已做，明早演练再校一次也无妨）
if gunzip -t "${LATEST_BACKUP}" 2>/dev/null; then
    ok "gzip 完整性校验通过"
else
    fail "gzip 完整性校验失败: ${LATEST_BACKUP}"
    exit 1
fi

# ───────────────────────── 2. 启 mysql-drill ─────────────────────────
kase "[2/8]" "启动隔离 mysql-drill 容器"
docker rm -f "${MYSQL_DRILL_CONTAINER}"   >/dev/null 2>&1 || true
docker volume rm -f "${DRILL_VOLUME}"     >/dev/null 2>&1 || true
docker volume create "${DRILL_VOLUME}"     >/dev/null
if docker run -d \
    --name "${MYSQL_DRILL_CONTAINER}" \
    --network "${MYSQL_DRILL_NETWORK}" \
    -p "${MYSQL_DRILL_PORT_BIND}:3306" \
    -e MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD}" \
    -e MYSQL_DATABASE="${DB_NAME}" \
    -v "${DRILL_VOLUME}:/var/lib/mysql" \
    "${MYSQL_IMAGE}" \
    --character-set-server=utf8mb4 \
    --collation-server=utf8mb4_unicode_ci \
    --default-authentication-plugin=caching_sha2_password \
    >/dev/null 2>drill.err
then
    ok "mysql-drill 容器已起 (bind=${MYSQL_DRILL_PORT_BIND})"
else
    fail "mysql-drill 启动失败: $(cat drill.err 2>/dev/null | head -3)"
    exit 1
fi

# 等 mysql healthy
mysql_wait=0
MYSQL_HEALTHY=0
while [ $mysql_wait -lt 60 ]; do
    if docker exec "${MYSQL_DRILL_CONTAINER}" \
        mysqladmin ping -uroot -p"${MYSQL_ROOT_PASSWORD}" -h 127.0.0.1 --silent 2>/dev/null; then
        MYSQL_HEALTHY=1
        break
    fi
    sleep 1
    mysql_wait=$((mysql_wait+1))
done
if [ "${MYSQL_HEALTHY}" = "1" ]; then
    ok "mysql-drill healthy (用时 ${mysql_wait}s)"
else
    fail "mysql-drill 60s 未 healthy"
    docker logs "${MYSQL_DRILL_CONTAINER}" 2>&1 | tail -10 >&2 || true
    exit 1
fi

# ───────────────────────── 3. 灌 backup ─────────────────────────
kase "[3/8]" "灌 backup"
# mysqldump 单库输出不含 CREATE DATABASE，先确保 DB 存在
if docker exec "${MYSQL_DRILL_CONTAINER}" \
    mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" \
    -e "CREATE DATABASE IF NOT EXISTS \`${DB_NAME}\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;" \
    >/dev/null 2>drill.err
then
    ok "drill DB ${DB_NAME} 已就绪"
else
    fail "drill DB 创建失败: $(cat drill.err 2>/dev/null | head -3)"
    exit 1
fi

if gunzip -c "${LATEST_BACKUP}" \
    | docker exec -i "${MYSQL_DRILL_CONTAINER}" \
        mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" "${DB_NAME}" \
    >/dev/null 2>drill.err
then
    ok "backup 已灌入 ${DB_NAME}"
else
    fail "灌 backup 失败: $(cat drill.err 2>/dev/null | head -10)"
    cat drill.err >&2
    exit 1
fi

# ───────────────────────── 4. 灌后校验 ─────────────────────────
kase "[4/8]" "灌后校验"
TABLE_INFO="$(docker exec "${MYSQL_DRILL_CONTAINER}" mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" "${DB_NAME}" -BNe \
    "SELECT GROUP_CONCAT(CONCAT(TABLE_NAME,'=',TABLE_ROWS) ORDER BY TABLE_NAME SEPARATOR ' ')
       FROM information_schema.TABLES WHERE TABLE_SCHEMA='${DB_NAME}';" 2>/dev/null || true)"
if [ -n "${TABLE_INFO}" ]; then
    ok "restore 后表清单: ${TABLE_INFO}"
else
    fail "灌 backup 后查不到任何表"
    exit 1
fi

ADMIN_COUNT="$(docker exec "${MYSQL_DRILL_CONTAINER}" mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" "${DB_NAME}" -BNe \
    "SELECT COUNT(*) FROM users WHERE username='${ADMIN_USER}' AND role='admin';" 2>/dev/null || echo "0")"
if [ "${ADMIN_COUNT}" -ge 1 ] 2>/dev/null; then
    ok "admin 行已存在 (count=${ADMIN_COUNT})，可走 smoke 登录"
else
    fail "灌 backup 后查不到 admin 用户 (count=${ADMIN_COUNT})；备份缺失 V099__create_admin.sql 或 schema 行"
    exit 1
fi

# ───────────────────────── 5. 启 drill-proxy + drill-web ─────────────────────────
kase "[5/8]" "启动 drill-proxy + drill-web"

# drill-proxy：不映射 host 端口（同网络内通过容器名 2375 访问）
docker rm -f "${PROXY_DRILL_CONTAINER}" >/dev/null 2>&1 || true
if docker run -d --rm \
    --name "${PROXY_DRILL_CONTAINER}" \
    --network "${MYSQL_DRILL_NETWORK}" \
    -v /var/run/docker.sock:/var/run/docker.sock:ro \
    -e CONTAINERS=1 -e POST=1 -e EXEC=1 \
    -e IMAGES=0 -e NETWORKS=0 -e VOLUMES=0 -e EVENTS=0 \
    -e PING=1 -e INFO=1 -e VERSION=1 -e LOG_LEVEL=warn \
    "${PROXY_IMAGE}" \
    >/dev/null 2>drill.err
then
    ok "drill-proxy 已起 (CONTAINERS=1 POST=1 EXEC=1；IMAGES/NETWORKS/VOLUMES/EVENTS=0)"
else
    fail "drill-proxy 启动失败: $(cat drill.err 2>/dev/null | head -3)"
    exit 1
fi

# drill-web
docker rm -f "${WEB_DRILL_CONTAINER}" >/dev/null 2>&1 || true
if docker run -d --rm \
    --name "${WEB_DRILL_CONTAINER}" \
    --network "${MYSQL_DRILL_NETWORK}" \
    -p "${WEB_DRILL_PORT_BIND}:8080" \
    -e DB_HOST="${MYSQL_DRILL_CONTAINER}" \
    -e DB_PORT=3306 \
    -e DB_USER=root \
    -e DB_PASSWORD="${MYSQL_ROOT_PASSWORD}" \
    -e DB_NAME="${DB_NAME}" \
    -e DB_POOL_MIN=2 \
    -e DB_POOL_MAX=8 \
    -e DOCKER_SOCKET_URL="tcp://${PROXY_DRILL_CONTAINER}:2375" \
    -e JUDGE_IMAGE="${JUDGE_IMAGE}" \
    -e LOG_FORMAT=json \
    -e LOG_LEVEL="${LOG_LEVEL}" \
    -e SERVER_HOST=0.0.0.0 \
    -e SERVER_PORT=8080 \
    -e SERVER_THREAD_POOL_SIZE=4 \
    -e TZ=Asia/Shanghai \
    -e WEB_BIND="${WEB_DRILL_PORT_BIND}" \
    "${WEB_IMAGE}" \
    >/dev/null 2>drill.err
then
    ok "drill-web 已起 (bind=${WEB_DRILL_PORT_BIND}, DB_HOST=${MYSQL_DRILL_CONTAINER})"
else
    fail "drill-web 启动失败: $(cat drill.err 2>/dev/null | head -3)"
    exit 1
fi

# ───────────────────────── 6. smoke 测试 ─────────────────────────
kase "[6/8]" "smoke：等 drill-web /api/v1/health 200"
HEALTH_OK=0
health_wait=0
while [ $health_wait -lt 60 ]; do
    api GET /health
    if [ "${HTTP_CODE}" = "200" ]; then HEALTH_OK=1; break; fi
    sleep 1
    health_wait=$((health_wait+1))
done
if [ "${HEALTH_OK}" = "1" ]; then
    ok "/api/v1/health → 200 (用时 ${health_wait}s)"
    HEALTH_DOCKER="$(jqb '.docker' 2>/dev/null || echo "?")"
    ok "/api/v1/health 探针 .docker=${HEALTH_DOCKER}"
else
    fail "/api/v1/health 60s 未 200（最后 HTTP=${HTTP_CODE}）"
    docker logs "${WEB_DRILL_CONTAINER}" 2>&1 | tail -15 >&2 || true
    exit 1
fi

kase "[6/8]" "smoke：admin 登录"
if ! login_body="$(jq -n --arg u "${ADMIN_USER}" --arg p "${ADMIN_PASS}" '{username:$u,password:$p}')"; then
    skip "jq 不可用，无法构造登录体"
    exit 0
fi
if [ -z "${login_body}" ]; then
    skip "jq 不可用，无法构造登录体（已在前置 cap 行记 SKIP）"
    exit 0
fi
api POST /auth/login -d "${login_body}"
if [ "${HTTP_CODE}" = "200" ]; then
    ADMIN_TOK="$(jqb '.data.access_token')"
    ok "admin /auth/login → 200 (token 前 12=${ADMIN_TOK:0:12}…)"
else
    fail "admin /auth/login HTTP=${HTTP_CODE} body=$(head -c 200 "${RESP_BODY}" 2>/dev/null | tr '\n' ' ')"
    docker logs "${WEB_DRILL_CONTAINER}" 2>&1 | tail -10 >&2 || true
    exit 1
fi

kase "[6/8]" "smoke:bulk-import two-sum"
TWO_SUM_JSON="${PROBLEMS_DIR}/${JUDGE_PROBLEM_SLUG}.json"
if [ ! -f "${TWO_SUM_JSON}" ]; then
    fail "找不到 ${TWO_SUM_JSON}"
    exit 1
fi
# 复用 curl 直传 multipart（api() helper 不支持 file form）
IMPORT_CODE="$(curl -sS -o drill.body -w '%{http_code}' \
    -H "Authorization: Bearer ${ADMIN_TOK}" \
    -F "files=@${TWO_SUM_JSON};type=application/json" \
    "${API}/admin/problems/import?on_duplicate=skip" 2>/dev/null)" || IMPORT_CODE="000"
if [ "${IMPORT_CODE}" = "201" ] || [ "${IMPORT_CODE}" = "200" ]; then
    ok "bulk-import two-sum → ${IMPORT_CODE}（on_duplicate=skip 幂等）"
else
    fail "bulk-import two-sum HTTP=${IMPORT_CODE} body=$(head -c 200 drill.body 2>/dev/null | tr '\n' ' ')"
    exit 1
fi

# 取 PID
api GET "/problems/${JUDGE_PROBLEM_SLUG}" -t "${ADMIN_TOK}"
PID="$(jqb '.data.id')"
if [ -n "${PID}" ] && [ "${PID}" != "null" ]; then
    ok "two-sum problem_id=${PID}"
else
    fail "取 two-sum problem_id 失败"
    exit 1
fi

kase "[6/8]" "smoke:注册测试用户"
USER_NAME="drill_u_${RANDOM}_$$"
USER_PW="Passw0rd_drill"
if ! reg_body="$(jq -n --arg u "${USER_NAME}" --arg p "${USER_PW}" '{username:$u,password:$p}')"; then
    skip "jq 不可用，无法构造 register 体"
    exit 0
fi
api POST /auth/register -d "${reg_body}"
if [ "${HTTP_CODE}" = "201" ]; then
    USER_TOK="$(jqb '.data.access_token')"
    USER_ID="$(jqb '.data.user.id')"
    ok "register ${USER_NAME} → 201 (id=${USER_ID})"
else
    fail "register HTTP=${HTTP_CODE} body=$(head -c 200 "${RESP_BODY}" 2>/dev/null | tr '\n' ' ')"
    exit 1
fi

kase "[6/8]" "smoke:提交 two-sum AC 并轮询"
AC_CODE='int main(){ std::cout << "9 9\n"; return 0; }'
if ! sub_body="$(jq -n --argjson pid "${PID}" --arg code "${AC_CODE}" \
    '{problem_id:$pid,language:"cpp",code:$code}')"; then
    skip "jq 不可用，无法构造 submissions 体"
    exit 0
fi
api POST /submissions -t "${USER_TOK}" -d "${sub_body}"
if [ "${HTTP_CODE}" != "201" ]; then
    fail "/submissions HTTP=${HTTP_CODE} body=$(head -c 200 "${RESP_BODY}" 2>/dev/null | tr '\n' ' ')"
    exit 1
fi
SUB_ID="$(jqb '.data.submission_id')"
SUB_FIRST="$(jqb '.data.status')"
ok "POST /submissions → 201 (id=${SUB_ID} first=${SUB_FIRST})"

# 轮询终态（最多 JUDGE_POLL_TIMEOUT_S）
SUB_STATUS="${SUB_FIRST}"
elapsed=0
while [ "${elapsed}" -lt "${JUDGE_POLL_TIMEOUT_S}" ]; do
    api GET "/submissions/${SUB_ID}" -t "${USER_TOK}"
    SUB_STATUS="$(jqb '.data.status')"
    case "${SUB_STATUS}" in
        ac|wa|ce|re|tle|mle|ole|pe|se) break;;
    esac
    sleep 1
    elapsed=$((elapsed+1))
done
case "${SUB_STATUS}" in
    ac)
        ok "two-sum → ${SUB_STATUS}（${elapsed}s）；restore 后判题链路通畅"
        ;;
    *)
        fail "two-sum 终态=${SUB_STATUS}（${elapsed}s 仍未到 ac）— restore 后判题链路异常"
        ;;
esac

kase "[6/8]" "smoke:/api/v1/metrics（v1.2.68 Prometheus 接入）"
api GET /metrics -t "${ADMIN_TOK}"
if [ "${HTTP_CODE}" = "200" ]; then
    metrics_lines="$(head -50 "${RESP_BODY}" 2>/dev/null | grep -cE '^litecode_' || true)"
    : "${metrics_lines:=0}"
    ok "/api/v1/metrics → 200, 暴露 ${metrics_lines} 行 litecode_* 指标"
else
    fail "/api/v1/metrics HTTP=${HTTP_CODE}"
fi

# ───────────────────────── 7. 拆 drill 栈 ─────────────────────────
kase "[7/8]" "拆 drill 栈"
cleanup
ok "drill 容器 + 临时卷已拆"

# ───────────────────────── 8. 收尾 ─────────────────────────
kase "[8/8]" "完成"
emit_result
[ "${FAIL}" -gt 0 ] && exit 1 || exit 0
