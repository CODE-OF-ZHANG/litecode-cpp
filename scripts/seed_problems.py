#!/usr/bin/env python3
"""v1.2.50: import 10 seed problems into the OJ via the bulk-import API.

Usage:
    python scripts/seed_problems.py \
        --base http://127.0.0.1:8080 \
        --user admin \
        --password 'admin123!' \
        --problems-dir problems

The script:
  1. logs in as admin and pulls the access token
  2. lists all *.json in --problems-dir
  3. multipart-uploads them as the `files` form field
     (per the contract in src/routes/admin_bulk_import_routes.h)
  4. prints the response summary (imported / skipped / failed counts)
  5. exits 0 on full success, 1 on any failure (so it can gate CI later)

Idempotent: by default ?on_duplicate=skip — re-running on an
already-seeded DB is a no-op (every file lands in `skipped`).
"""
import argparse
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--base', default='http://127.0.0.1:8080',
                   help='OJ API base URL')
    p.add_argument('--user', default='admin')
    p.add_argument('--password', default='admin123!')
    p.add_argument('--problems-dir', default='problems')
    p.add_argument('--on-duplicate', default='skip',
                   choices=['skip', 'overwrite'])
    args = p.parse_args()

    base = args.base.rstrip('/')

    # ── 1. login ──────────────────────────────────────────────
    print(f'[seed] logging in as {args.user!r} on {base} ...')
    login_req = urllib.request.Request(
        f'{base}/api/v1/auth/login',
        data=json.dumps({'username': args.user,
                          'password': args.password}).encode(),
        headers={'Content-Type': 'application/json'},
        method='POST')
    try:
        with urllib.request.urlopen(login_req, timeout=10) as r:
            login = json.load(r)
    except urllib.error.HTTPError as e:
        print(f'[seed] FAIL: login HTTP {e.code}: {e.read().decode()[:200]}')
        return 1
    except Exception as e:
        print(f'[seed] FAIL: login {e}')
        return 1
    token = login['data']['access_token']
    user = login['data']['user']
    if user['role'] != 'admin':
        print(f'[seed] FAIL: user {user["username"]!r} is not admin')
        return 1
    print(f'[seed] OK: token={token[:8]}... role={user["role"]}')

    # ── 2. discover problem files ──────────────────────────────
    pdir = Path(args.problems_dir)
    if not pdir.is_dir():
        print(f'[seed] FAIL: {pdir} is not a directory')
        return 1
    files = sorted(pdir.glob('*.json'))
    if not files:
        print(f'[seed] FAIL: no .json files in {pdir}')
        return 1
    print(f'[seed] found {len(files)} problem files in {pdir}')

    # ── 3. multipart upload ───────────────────────────────────
    boundary = '----LiteCodeSeedBoundary7f3a9d2b'
    body = bytearray()
    for path in files:
        if not path.name.lower().endswith('.json'):
            continue
        file_bytes = path.read_bytes()
        body += f'--{boundary}\r\n'.encode()
        body += (f'Content-Disposition: form-data; name="files"; '
                 f'filename="{path.name}"\r\n').encode()
        body += b'Content-Type: application/json\r\n\r\n'
        body += file_bytes
        body += b'\r\n'
    body += f'--{boundary}--\r\n'.encode()

    url = (f'{base}/api/v1/admin/problems/import'
           f'?on_duplicate={args.on_duplicate}')
    import_req = urllib.request.Request(
        url,
        data=bytes(body),
        headers={
            'Authorization': f'Bearer {token}',
            'Content-Type': f'multipart/form-data; boundary={boundary}',
        },
        method='POST')

    try:
        with urllib.request.urlopen(import_req, timeout=60) as r:
            resp = json.load(r)
    except urllib.error.HTTPError as e:
        body = e.read().decode()[:400]
        print(f'[seed] FAIL: import HTTP {e.code}: {body}')
        return 1
    except Exception as e:
        print(f'[seed] FAIL: import {e}')
        return 1

    # ── 4. summary ─────────────────────────────────────────────
    data = resp.get('data', {})
    summary = data.get('summary', {})
    print(f'[seed] response summary: {json.dumps(summary, ensure_ascii=False)}')

    failed = summary.get('failed', 0)
    if failed:
        print(f'[seed] {failed} file(s) failed:')
        for f in data.get('failures', []):
            print(f'  - {f.get("filename")}: '
                  f'{f.get("stage")}: {f.get("reason")} '
                  f'({f.get("details")})')
        return 1

    imported = data.get('imported', [])
    print(f'[seed] imported {len(imported)} problems:')
    for p in imported:
        slug = p.get('slug', '?')
        title = p.get('title', '?')
        diff = p.get('action', '?')
        print(f'  - {slug:<35} {diff:<10} {title}')

    print('[seed] OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
