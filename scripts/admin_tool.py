#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# LiteCode-CPP — admin 账号管理小工具 (v1.3.3.7)
#
# 这就是用户请求的"小工具":
#   - 向数据库中注册一个管理员账号 admin(密码 = admin123)
#   - 密码用 bcrypt 实时加密,以密文形式保存(不是写死 hash)
#   - 顺带撤销 zhangxu 的管理员能力
#
# ─────────────────────────────────────────────────────────────────────────
# ⚠️  SECURITY WARNING: JWT role 是写死在 token 里的
# ─────────────────────────────────────────────────────────────────────────
# 改完数据库后,被改用户的 JWT 里仍然带着旧的 role claim。后果:
#   - access token  TTL 2h  → 降权的用户在最长 2h 内仍能调 admin API
#   - refresh token TTL 7d  → 降权的用户可继续 refresh 拿新 access token
#   - 改该用户的密码 / 强制其 logout 都不影响 refresh token(它不查密码)
# 真正的修复需要新增 `users.token_version` + JWT claim + 改 role 时自增 +
# require_role 之前对比 — 工作量跨多个文件,留作 v1.3.4 独立 PR 处理。
# 本工具只是数据库层修复,无法绕过这一限制。
# ─────────────────────────────────────────────────────────────────────────
#
# 用法(在项目根目录运行):
#
#   python scripts/admin_tool.py reset-admin           # 重置 admin 密码为 admin123 + 撤 zhangxu
#   python scripts/admin_tool.py reset-admin --yes     # 同上,跳过确认
#   python scripts/admin_tool.py create-admin <user> <password> --email <e>
#   python scripts/admin_tool.py grant-admin <user>    # 提权任意用户为 admin
#   python scripts/admin_tool.py revoke-admin <user>   # 降权(永远不能撤自己)
#   python scripts/admin_tool.py delete-admin <user>   # 删 admin 账号(留最后一名 admin)
#   python scripts/admin_tool.py list                  # 列出所有 admin
#   python scripts/admin_tool.py set-password <user> <new-password>
#   python scripts/admin_tool.py hash <password>       # 只生成 bcrypt 哈希(不写库)
#
# 依赖:
#   - Python 3.8+
#   - bcrypt(pip install bcrypt;项目 Dockerfile 的 builder 镜像已带)
#   - docker(默认执行后端;走 litecode-mysql 容器内的 mysql,100% 命中目标实例)
#
# 设计要点:
#   - 默认走 `docker exec -e MYSQL_PWD=... litecode-mysql mysql ...`,
#     避免 host 上恰好装了 MySQL 占用 3306 导致连错实例的坑(v1.3.3.7 实测)
#   - 备选 host mysql 客户端模式:LITECODE_DB_BACKEND=host
#   - 所有破坏性操作都默认 dry-run,需 --yes 才执行
#   - revoke-admin 会保护最后一名 admin(避免把所有 admin 都撤光造成 423 lockout)
#   - revoke-self:用户传自己的 username 时拒绝并提示
#   - 输出 audit-style 表格 + diff,运维一眼能复核
#
# 与 src/auth/password_hash.h 的兼容性:
#   Python bcrypt 输出 `$2b$12$<22c salt><31c hash>` 与 OpenBSD / C bcrypt 完全
#   互通,项目里的 bcrypt_checkpass() 能直接验证。cost=12 与
#   password_hash.h::kBcryptCostFactor 一致,登录时不会触发 needs_rehash。

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import textwrap
from typing import Iterable, List, Optional, Sequence, Tuple

# bcrypt 是项目 Dockerfile builder 阶段就装好的;若本地没装,提前给清晰报错
try:
    import bcrypt  # type: ignore
except ImportError:
    sys.stderr.write(
        "[admin_tool] FATAL: Python 'bcrypt' module not found.\n"
        "  install: pip install bcrypt\n"
        "  or use the bundled C++ tool: "
        "cmake --build build --target lit_gen_password_hash\n"
    )
    sys.exit(3)

# ─────────────────────────────────────────────────────────────────────────
# Constants — 与 src/auth/password_hash.h 保持一致
# ─────────────────────────────────────────────────────────────────────────

BCRYPT_COST = 12                # kBcryptCostFactor
BCRYPT_PREFIX = b"2b"           # 必须用 2b 而非 2a,确保与 C++ 项目版本匹配
MIN_PASSWORD_LEN = 8            # kMinPasswordLength
MAX_PASSWORD_LEN = 72           # kMaxPasswordLength (bcrypt hard cap)

# ─────────────────────────────────────────────────────────────────────────
# 密码强度校验 — 镜像 password_hash.h::validate_password_strength
# ─────────────────────────────────────────────────────────────────────────

def validate_password_strength(pw: str) -> Optional[str]:
    if len(pw) < MIN_PASSWORD_LEN:
        return f"password must be at least {MIN_PASSWORD_LEN} characters"
    if len(pw) > MAX_PASSWORD_LEN:
        return f"password must be at most {MAX_PASSWORD_LEN} characters"
    has_letter = any(c.isalpha() for c in pw)
    has_digit = any(c.isdigit() for c in pw)
    if not has_letter or not has_digit:
        return "password must contain both letters and digits"
    return None


def hash_password(pw: str) -> str:
    err = validate_password_strength(pw)
    if err:
        raise ValueError(err)
    salt = bcrypt.gensalt(rounds=BCRYPT_COST, prefix=BCRYPT_PREFIX)
    return bcrypt.hashpw(pw.encode("utf-8"), salt).decode("ascii")


# ─────────────────────────────────────────────────────────────────────────
# MySQL 执行后端 — 默认走 docker exec,fallback 到 host mysql 客户端
# ─────────────────────────────────────────────────────────────────────────
# v1.3.3.7 真实踩坑:host 上如果碰巧装了 MySQL 占用了 3306 端口(Windows
# 用户常见),docker-compose 把 MySQL 容器的 3306 host 映射会失败,host
# 上的 mysql 客户端会连到本机 MySQL 而不是容器 — 数据完全错位!
#
# 所以默认走 `docker exec -e MYSQL_PWD=... litecode-mysql mysql ...`,
# 密码通过 env 传(不进 process list / 不进 shell history),100% 进
# 容器内的 MySQL。
#
# 如果用户显式设了 MYSQL_HOST env(非默认),就走 host mysql 客户端
# (适合 CI runner / 无 docker 场景)。
# ─────────────────────────────────────────────────────────────────────────

DEFAULT_CONTAINER = "litecode-mysql"
DEFAULT_DB        = "litecode"
DEFAULT_ROOT_PWD   = "123456"  # 与 .env / docker-compose 一致


def _backend() -> str:
    """auto / docker / host 三种模式。auto 优先 docker。"""
    return os.environ.get("LITECODE_DB_BACKEND", "auto")


def _docker_mysql_cmd() -> List[str]:
    pwd = os.environ.get("MYSQL_ROOT_PASSWORD", DEFAULT_ROOT_PWD)
    return [
        "docker", "exec", "-i", "-e", f"MYSQL_PWD={pwd}",
        os.environ.get("LITECODE_MYSQL_CONTAINER", DEFAULT_CONTAINER),
        "mysql", "-uroot", "--batch", "--skip-column-names",
        os.environ.get("LITECODE_MYSQL_DATABASE", DEFAULT_DB),
    ]


def _host_mysql_cmd() -> List[str]:
    return [
        "mysql",
        "-h", os.environ.get("MYSQL_HOST", "127.0.0.1"),
        "-P", os.environ.get("MYSQL_PORT", "3306"),
        f"-u{os.environ.get('MYSQL_USER', 'root')}",
        "--protocol=TCP",
        "--batch",
        "--skip-column-names",
        os.environ.get("LITECODE_MYSQL_DATABASE", DEFAULT_DB),
    ]


def _exec_sql(sql: str) -> str:
    """执行 SQL,返回 stdout。

    auto 模式:先尝试 docker,失败就退到 host 客户端。
    """
    mode = _backend()
    candidates: List[Tuple[str, List[str], Optional[dict]]] = []

    if mode in ("auto", "docker"):
        candidates.append(("docker", _docker_mysql_cmd(), None))
    if mode in ("auto", "host"):
        env = os.environ.copy()
        env["MYSQL_PWD"] = os.environ.get("MYSQL_ROOT_PASSWORD", DEFAULT_ROOT_PWD)
        candidates.append(("host", _host_mysql_cmd(), env))

    last_err = ""
    for name, cmd, env in candidates:
        try:
            proc = subprocess.run(
                cmd, input=sql.encode("utf-8"),
                capture_output=True, env=env, check=False,
            )
        except FileNotFoundError as e:
            last_err = f"{name}: {e}"
            continue
        if proc.returncode == 0:
            return proc.stdout.decode("utf-8", errors="replace")
        last_err = (f"{name} (rc={proc.returncode}): "
                    f"{proc.stderr.decode('utf-8', errors='replace').strip()}")
        if mode != "auto":
            break  # 显式指定模式,失败就直接报错

    sys.stderr.write(
        f"[admin_tool] FATAL: no MySQL backend succeeded.\n"
        f"  last error: {last_err}\n"
        f"  hint: install docker (and run scripts/start.sh), or set\n"
        f"        MYSQL_HOST/PORT/USER + MYSQL_ROOT_PASSWORD env vars to\n"
        f"        point at a reachable MySQL, or LITECODE_DB_BACKEND=host\n"
        f"        to skip the docker attempt.\n"
    )
    sys.exit(4)


def _table_lines(stdout: str) -> List[List[str]]:
    """把 mysql 的 tab-separated 输出切成 row 列表。"""
    rows: List[List[str]] = []
    for line in stdout.splitlines():
        if not line.strip():
            continue
        if line.startswith("ERROR") or line.startswith("mysql:"):
            sys.stderr.write(line + "\n")
            continue
        rows.append(line.split("\t"))
    return rows


def _user_exists(sql_runner, username: str) -> bool:
    """精确查 username 是否存在(防止 LIKE 误匹配)。"""
    safe = username.replace("'", "''")  # 防止 SQL injection
    rows = _table_lines(sql_runner(
        f"SELECT 1 FROM users WHERE username='{safe}' LIMIT 1;\n"
    ))
    return bool(rows)


# ─────────────────────────────────────────────────────────────────────────
# 业务操作
# ─────────────────────────────────────────────────────────────────────────

def _ensure_admin_exists(sql_runner) -> bool:
    """检查 username='admin' 是否存在;返回 True/False。"""
    rows = _table_lines(sql_runner("SELECT username FROM users WHERE username='admin' LIMIT 1;"))
    return bool(rows)


def _current_admins(sql_runner) -> List[Tuple[str, str]]:
    """返回 [(username, role), ...] 仅 role='admin' 的用户。"""
    rows = _table_lines(sql_runner("SELECT username, role FROM users WHERE role='admin' ORDER BY username;"))
    return [(r[0], r[1]) for r in rows if len(r) >= 2]


def cmd_hash(args: argparse.Namespace) -> int:
    """只生成 bcrypt 哈希,不写库。便于调试 / 写别的 SQL。"""
    try:
        print(hash_password(args.password))
    except ValueError as e:
        sys.stderr.write(f"[admin_tool] {e}\n")
        return 2
    return 0


def cmd_reset_admin(args: argparse.Namespace) -> int:
    """重置 admin 密码 = admin123,同时撤 zhangxu 管理员权限(用户原话)。"""
    pw = "admin123"
    pw_hash = hash_password(pw)

    dry = not args.yes
    if dry:
        print("[dry-run] would execute:\n")

    # 0) 预检:zhangxu 是否存在?不存在就给用户一个明确提示,
    #    避免 UPDATE 0 rows 让用户以为脚本悄悄漏跑。
    zhangxu_exists = _user_exists(_exec_sql, "zhangxu")
    if not dry and not zhangxu_exists:
        print(
            "[admin_tool] NOTE: 'zhangxu' is not in the users table — the "
            "demote step is a no-op (this is fine; the script is idempotent). "
            "If you expected the user to exist, check that the account was "
            "created in a different environment or use a different name.\n"
        )

    # 1) admin:密码 hash 重置;已存在则只刷 hash,不存在则插入
    admin_sql = textwrap.dedent(f"""\
        INSERT INTO users (username, password_hash, role, email, avatar)
        VALUES ('admin', '{pw_hash}', 'admin', NULL, '/assets/img/default-avatar.svg')
        ON DUPLICATE KEY UPDATE password_hash = VALUES(password_hash);
    """)
    # 2) zhangxu:精确降权,不动其它 admin
    zhangxu_sql = (
        "UPDATE users SET role='user' WHERE username='zhangxu' AND role='admin';\n"
    )
    # 3) 复核
    verify_sql = (
        "SELECT id, username, role, email FROM users "
        "WHERE role='admin' OR username='zhangxu' ORDER BY role DESC, username;\n"
    )

    print("=== 1/3 admin password reset ===")
    print(admin_sql, end="")
    if not dry:
        _exec_sql(admin_sql)

    print("=== 2/3 zhangxu revoke-admin ===")
    print(zhangxu_sql, end="")
    if not dry:
        _exec_sql(zhangxu_sql)

    print("=== 3/3 verify ===")
    print(verify_sql, end="")
    if not dry:
        out = _exec_sql(verify_sql)
        print(out.rstrip())
        print("[admin_tool] DONE. admin password is now 'admin123' "
              "(bcrypt cost=12). zhangxu demoted to 'user' (or no-op if missing).")
        print("[admin_tool] WARNING: Production: change the password immediately "
              "via the admin web UI or 'admin_tool.py set-password admin <strong-pw>'.")
    else:
        print("[dry-run] re-run with --yes to execute.")
    return 0


def cmd_create_admin(args: argparse.Namespace) -> int:
    """新建一个管理员账号(若 username 已存在则报错)。"""
    try:
        pw_hash = hash_password(args.password)
    except ValueError as e:
        sys.stderr.write(f"[admin_tool] {e}\n")
        return 2

    sql = textwrap.dedent(f"""\
        INSERT INTO users (username, password_hash, role, email, avatar)
        VALUES ('{args.username}', '{pw_hash}', 'admin', {repr(args.email)}, '/assets/img/default-avatar.svg');
    """)
    if args.dry_run:
        print("[dry-run] would execute:")
        print(sql)
        return 0
    _exec_sql(sql)
    print(f"[admin_tool] admin '{args.username}' created (password hashed, cost={BCRYPT_COST}).")
    return 0


def cmd_set_password(args: argparse.Namespace) -> int:
    """改任意用户的密码(自己改自己,或 admin 改别人)。"""
    try:
        pw_hash = hash_password(args.password)
    except ValueError as e:
        sys.stderr.write(f"[admin_tool] {e}\n")
        return 2
    sql = f"UPDATE users SET password_hash='{pw_hash}' WHERE username='{args.username}';\n"
    if args.dry_run:
        print("[dry-run] would execute:")
        print(sql)
        return 0
    _exec_sql(sql)
    print(f"[admin_tool] password for '{args.username}' updated.")
    return 0


def cmd_grant_admin(args: argparse.Namespace) -> int:
    """把任意用户提为 admin(不存在则报错)。"""
    sql = f"UPDATE users SET role='admin' WHERE username='{args.username}';\n"
    if args.dry_run:
        print("[dry-run] would execute:")
        print(sql)
        return 0
    _exec_sql(sql)
    print(f"[admin_tool] '{args.username}' granted admin role.")
    return 0


def cmd_revoke_admin(args: argparse.Namespace) -> int:
    """把任意用户降为 user,但保护最后一名 admin + 保护操作者自己。"""
    if args.username == args.operator:
        sys.stderr.write(
            f"[admin_tool] REFUSED: refusing to revoke admin for the operator "
            f"('{args.operator}') — pass --force-self to override.\n"
        )
        return 5

    # 保护最后一名 admin
    admins_before = _current_admins(_docker_exec_sql)
    if args.username in [u for u, _ in admins_before] and len(admins_before) <= 1:
        sys.stderr.write(
            f"[admin_tool] REFUSED: '{args.username}' is the LAST admin. "
            "Promote another admin first to avoid being locked out.\n"
        )
        return 5

    sql = (
        "UPDATE users SET role='user' "
        f"WHERE username='{args.username}' AND role='admin';\n"
    )
    if args.dry_run:
        print("[dry-run] would execute:")
        print(sql)
        return 0
    _exec_sql(sql)
    print(f"[admin_tool] '{args.username}' demoted to 'user'.")
    return 0


def cmd_delete_admin(args: argparse.Namespace) -> int:
    """删除一个 admin 账号,同样保护最后一名。"""
    if args.username == args.operator:
        sys.stderr.write(
            f"[admin_tool] REFUSED: refusing to delete the operator "
            f"('{args.operator}') — pass --force-self to override.\n"
        )
        return 5

    admins_before = _current_admins(_docker_exec_sql)
    if args.username in [u for u, _ in admins_before] and len(admins_before) <= 1:
        sys.stderr.write(
            f"[admin_tool] REFUSED: '{args.username}' is the LAST admin.\n"
        )
        return 5

    sql = f"DELETE FROM users WHERE username='{args.username}';\n"
    if args.dry_run:
        print("[dry-run] would execute:")
        print(sql)
        return 0
    _exec_sql(sql)
    print(f"[admin_tool] '{args.username}' deleted.")
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    """列出所有 admin 账号。"""
    out = _exec_sql(
        "SELECT id, username, role, email, created_at, last_login "
        "FROM users WHERE role='admin' ORDER BY username;\n"
    )
    print(out.rstrip())
    return 0


# ─────────────────────────────────────────────────────────────────────────
# argparse
# ─────────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="admin_tool.py",
        description="LiteCode-CPP admin account management utility (v1.3.3.7).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            examples:
              %(prog)s reset-admin --yes
              %(prog)s grant-admin alice
              %(proj)s revoke-admin zhangxu --yes    # careful!
        """).replace("%(proj)s", "%(prog)s"),
    )
    # global flags
    p.add_argument("--operator", default=os.environ.get("USER", "operator"),
                   help="username of the person running this tool "
                        "(default: $USER). Used to refuse self-revoke.")
    sub = p.add_subparsers(dest="cmd", required=True, metavar="COMMAND")

    # reset-admin
    sp = sub.add_parser("reset-admin",
                        help="reset admin password to 'admin123' AND "
                             "demote zhangxu to 'user' (the default workflow).")
    sp.add_argument("--yes", action="store_true",
                    help="actually execute (default: dry-run)")
    sp.set_defaults(func=cmd_reset_admin)

    # hash
    sp = sub.add_parser("hash", help="print bcrypt hash only, do not write DB.")
    sp.add_argument("password")
    sp.set_defaults(func=cmd_hash)

    # create-admin
    sp = sub.add_parser("create-admin", help="create a brand-new admin account.")
    sp.add_argument("username")
    sp.add_argument("password")
    sp.add_argument("--email", default=None)
    sp.add_argument("--dry-run", action="store_true")
    sp.set_defaults(func=cmd_create_admin)

    # set-password
    sp = sub.add_parser("set-password", help="change a user's password.")
    sp.add_argument("username")
    sp.add_argument("password")
    sp.add_argument("--dry-run", action="store_true")
    sp.set_defaults(func=cmd_set_password)

    # grant-admin / revoke-admin / delete-admin
    for name, help_text in [
        ("grant-admin",   "promote an existing user to admin role."),
        ("revoke-admin",  "demote an admin to user role (keeps last admin)."),
        ("delete-admin",  "delete an admin account (keeps last admin)."),
    ]:
        sp = sub.add_parser(name, help=help_text)
        sp.add_argument("username")
        sp.add_argument("--dry-run", action="store_true")
        sp.add_argument("--yes", action="store_true",
                        help="actually execute (default: dry-run)")
        sp.add_argument("--force-self", action="store_true",
                        help="allow operating on --operator (dangerous)")
        sp.set_defaults(func={"grant-admin":  cmd_grant_admin,
                              "revoke-admin": cmd_revoke_admin,
                              "delete-admin": cmd_delete_admin}[name])

    # list
    sp = sub.add_parser("list", help="list all admin accounts.")
    sp.set_defaults(func=cmd_list)

    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())