#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — log aggregation stack linter (Phase 9)
# =============================================================
# Checks the mandatory Docker JSON logging contract and the optional
# Loki/Promtail profile without starting containers.
# =============================================================
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${REPO_ROOT}/docker-compose.yml"
LOKI_CONFIG="${REPO_ROOT}/monitoring/loki/loki-config.yml"
PROMTAIL_CONFIG="${REPO_ROOT}/monitoring/promtail/promtail-config.yml"
DATASOURCES="${REPO_ROOT}/monitoring/grafana/datasources.yml"

if [ -t 1 ]; then
  RED=$'\033[31m'; GRN=$'\033[32m'; YLW=$'\033[33m'; NC=$'\033[0m'
else
  RED=''; GRN=''; YLW=''; NC=''
fi
errs=0
ok=0
warns=0
log_err()  { echo "${RED}ERR${NC}  $*" >&2; errs=$((errs+1)); }
log_warn() { echo "${YLW}WARN${NC} $*";                   warns=$((warns+1)); }
log_ok()   { echo "${GRN}OK${NC}   $*";                   ok=$((ok+1)); }

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    log_err "缺少依赖: $1"
    return 1
  fi
  return 0
}

for cmd in docker jq; do
  require_cmd "$cmd" || true
done

for file in "$COMPOSE_FILE" "$LOKI_CONFIG" "$PROMTAIL_CONFIG" "$DATASOURCES"; do
  if [ -f "$file" ]; then
    log_ok "文件存在: ${file#"$REPO_ROOT"/}"
  else
    log_err "文件不存在: ${file#"$REPO_ROOT"/}"
  fi
done

compose_json=''
if command -v docker >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; then
  if compose_json="$(cd "$REPO_ROOT" && docker compose --profile logging config --format json 2>/dev/null)"; then
    log_ok "docker compose logging profile 可解析"

    if jq -e '
      .services.web.logging.driver == "json-file" and
      .services.web.logging.options["max-size"] == "10m" and
      .services.web.logging.options["max-file"] == "3"
    ' <<<"$compose_json" >/dev/null; then
      log_ok "Docker stdout 接管: json-file / max-size=10m / max-file=3"
    else
      log_err "Docker logging contract 不符合 json-file / 10m / 3"
    fi

    if jq -e '
      (.services.loki.profiles | index("logging") != null) and
      (.services.promtail.profiles | index("logging") != null) and
      (.services.loki.image | startswith("grafana/loki:")) and
      (.services.promtail.image | startswith("grafana/promtail:"))
    ' <<<"$compose_json" >/dev/null; then
      log_ok "Loki/Promtail 已挂入 logging profile"
    else
      log_err "Loki/Promtail logging profile 配置缺失"
    fi

    if jq -e '
      any(.services.promtail.volumes[];
        (.source == "/var/run/docker.sock" and .read_only == true)) and
      any(.services.promtail.volumes[];
        (.source == "/var/lib/docker/containers" and .read_only == true))
    ' <<<"$compose_json" >/dev/null; then
      log_ok "Promtail 只读采集 Docker socket/JSON 日志目录"
    else
      log_err "Promtail Docker 采集挂载不是只读或缺失"
    fi
  else
    log_err "docker compose --profile logging config 失败"
  fi
else
  log_warn "docker/jq 不可用，跳过 Compose 渲染检查"
fi

# The mounted configs are intentionally checked structurally here as well as
# through Compose: Compose cannot parse files that are mounted into containers.
PYTHON_BIN=''
for candidate in python3 python; do
  if command -v "$candidate" >/dev/null 2>&1 \
      && "$candidate" -c 'import yaml' >/dev/null 2>&1; then
    PYTHON_BIN="$candidate"
    break
  fi
done

if [ -n "$PYTHON_BIN" ]; then
  if "$PYTHON_BIN" - "$LOKI_CONFIG" "$PROMTAIL_CONFIG" "$DATASOURCES" <<'PY'
import sys
import yaml
for filename in sys.argv[1:]:
    with open(filename, encoding="utf-8") as stream:
        yaml.safe_load(stream)
PY
  then
    log_ok "Loki/Promtail/Grafana YAML 可解析"
  else
    log_err "Loki/Promtail/Grafana YAML 解析失败"
  fi
else
  log_warn "python3/python 缺少 PyYAML，改用结构契约检查"
fi

if [ -f "$LOKI_CONFIG" ]; then
  if grep -Eq '^auth_enabled:[[:space:]]+false$' "$LOKI_CONFIG" \
      && grep -Eq 'http_listen_port:[[:space:]]+3100' "$LOKI_CONFIG" \
      && grep -Eq 'schema:[[:space:]]+v13' "$LOKI_CONFIG" \
      && grep -Eq 'retention_enabled:[[:space:]]+true' "$LOKI_CONFIG"; then
    log_ok "Loki 配置包含 HTTP/schema/retention 契约"
  else
    log_err "Loki 配置缺少必需字段"
  fi
fi

if [ -f "$PROMTAIL_CONFIG" ]; then
  if grep -Eq 'url:[[:space:]]+http://loki:3100/loki/api/v1/push' "$PROMTAIL_CONFIG" \
      && grep -Eq 'host:[[:space:]]+unix:///var/run/docker.sock' "$PROMTAIL_CONFIG" \
      && grep -Eq 'action:[[:space:]]+keep' "$PROMTAIL_CONFIG" \
      && grep -Eq 'replacement:[[:space:]]+/var/lib/docker/containers/\$1/\$1-json\.log' "$PROMTAIL_CONFIG" \
      && grep -Eq 'request_id:[[:space:]]+request_id' "$PROMTAIL_CONFIG"; then
    log_ok "Promtail client/discovery/filter/Docker JSON/request_id 字段契约存在"
  else
    log_err "Promtail 配置缺少采集契约"
  fi

  if awk '
    /^[[:space:]]*-[[:space:]]*labels:/ { in_labels=1; next }
    in_labels && /^[[:space:]]*-[[:space:]]*[a-zA-Z_]+:/ { in_labels=0 }
    in_labels && /^[[:space:]]+request_id:/ { found=1 }
    END { exit found ? 1 : 0 }
  ' "$PROMTAIL_CONFIG"; then
    log_ok "request_id 保持字段而非 Loki label"
  else
    log_err "request_id 不得配置为 Loki label"
  fi
fi

if [ -f "$DATASOURCES" ]; then
  if awk '
    /^[[:space:]]*-[[:space:]]*name:[[:space:]]*Loki[[:space:]]*$/ { in_loki=1; next }
    in_loki && /^[[:space:]]*-[[:space:]]*name:/ { in_loki=0 }
    in_loki && /^[[:space:]]+uid:[[:space:]]*loki[[:space:]]*$/ { found=1; exit }
    END { exit found ? 0 : 1 }
  ' "$DATASOURCES"; then
    log_ok "Grafana Loki datasource uid=loki"
  else
    log_err "Grafana Loki datasource 缺少 uid=loki"
  fi
fi

echo
echo "─── 总结 ────────────────"
echo "  OK   = ${ok}"
echo "  WARN = ${warns}"
echo "  ERR  = ${errs}"
echo

if [ "$errs" -gt 0 ]; then
  echo "${RED}FAIL${NC}: ${errs} 个错误需修复"
  exit 1
fi
echo "${GRN}PASS${NC}: 日志聚合配置校验通过"
exit 0
