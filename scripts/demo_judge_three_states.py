#!/usr/bin/env python3
"""v1.2.50-b: walk through the real judge container with 3 codes.

Submits three C++ programs against the two-sum problem and waits
for the judge container to run them to terminal states:

  - AC:  O(n) hash-map solution          → accepted
  - WA:  off-by-one bug (returns i+1, j) → wrong answer
  - TLE: while(true) {}                  → time limit exceeded

Each submission is async (the API returns submission_id immediately);
the script polls GET /api/v1/submissions/:id until status is
terminal. After all three, the script dumps the final DB state so
you can verify AC/WA/TLE landed as expected.

Usage:
    python scripts/demo_judge_three_states.py \
        --base http://127.0.0.1:8080 \
        --user admin \
        --password 'admin123!' \
        --problem-slug two-sum
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


# ── 1. AC: O(n) two-sum using unordered_map ───────────────────
# Adapted to the two-sum.json test-case format: stdin is
# "<n space-separated integers>\n<target>\n" (no leading count,
# the judge implicitly knows n = size-1 from the test data, but
# solutions just scan all ints and treat the last as target).
# Solutions print 0-indexed positions (per the samples).
CODE_AC = r"""#include <bits/stdc++.h>
using namespace std;
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    vector<int> a;
    int x; int target = 0;
    while (cin >> x) {
        a.push_back(x);
    }
    if (a.empty()) return 1;
    target = a.back();
    a.pop_back();
    int n = (int)a.size();
    unordered_map<int,int> seen;
    for (int i = 0; i < n; i++) {
        int need = target - a[i];
        auto it = seen.find(need);
        if (it != seen.end()) {
            cout << it->second << " " << i << "\n";
            return 0;
        }
        seen[a[i]] = i;
    }
    return 0;
}
"""

# ── 2. WA: off-by-one — returns (i+1, j+1) when problem uses 0-indexed ─
CODE_WA = r"""#include <bits/stdc++.h>
using namespace std;
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    vector<int> a;
    int x;
    while (cin >> x) a.push_back(x);
    if (a.empty()) return 1;
    int target = a.back();
    a.pop_back();
    int n = (int)a.size();
    // Bug: prints 1-indexed positions when the problem expects
    // 0-indexed → off-by-one output for every test case.
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (a[i] + a[j] == target) {
                cout << (i + 1) << " " << (j + 1) << "\n";
                return 0;
            }
    return 0;
}
"""

# ── 3. TLE: infinite loop (judge.sh enforces run_timeout) ──────
CODE_TLE = r"""#include <bits/stdc++.h>
using namespace std;
int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    vector<int> a;
    int x;
    while (cin >> x) a.push_back(x);
    // TLE: busy-wait forever; the judge.sh wrapper kills the
    // child via `timeout` after time_limit_ms.
    volatile long long s = 0;
    while (true) s++;
    return 0;
}
"""


def http(method, url, *, headers=None, data=None, timeout=30):
    req = urllib.request.Request(url, method=method,
                                 headers=headers or {},
                                 data=data)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        body = e.read().decode()[:300]
        raise RuntimeError(f'{method} {url} → HTTP {e.code}: {body}')


def login(base, user, password):
    _, j = http('POST', f'{base}/api/v1/auth/login',
                headers={'Content-Type': 'application/json'},
                data=json.dumps({'username': user, 'password': password}).encode())
    return j['data']['access_token']


def get_problem_id(base, token, slug):
    """Look up problem_id by slug via the public problems list."""
    _, j = http('GET', f'{base}/api/v1/problems?limit=200')
    for p in j['data']['items']:
        if p['slug'] == slug:
            return p['id']
    raise RuntimeError(f'no problem with slug {slug!r}')


def submit(base, token, problem_id, language, code):
    _, j = http('POST', f'{base}/api/v1/submissions',
                headers={'Authorization': f'Bearer {token}',
                         'Content-Type': 'application/json'},
                data=json.dumps({'problem_id': problem_id,
                                 'language': language,
                                 'code': code}).encode(),
                timeout=15)
    return j['data']['submission_id']


def poll_until_terminal(base, token, sid, timeout_sec=60):
    """Poll the submission until status ∈ {ac, wa, tle, mle, ole, re, ce, se}.
    Returns (final_status, runtime_ms, memory_kb)."""
    deadline = time.time() + timeout_sec
    last_status = None
    while time.time() < deadline:
        try:
            _, j = http('GET', f'{base}/api/v1/submissions/{sid}',
                        headers={'Authorization': f'Bearer {token}'},
                        timeout=10)
            d = j['data']
            last_status = d['status']
            if last_status not in ('pending', 'running'):
                return (last_status, d.get('runtime_ms'),
                        d.get('memory_kb'))
        except Exception as e:
            print(f'  [poll] transient: {e}')
        time.sleep(1.0)
    raise RuntimeError(f'submission {sid} stuck in {last_status} after '
                       f'{timeout_sec}s')


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--base', default='http://127.0.0.1:8080')
    p.add_argument('--user', default='admin')
    p.add_argument('--password', default='admin123!')
    p.add_argument('--problem-slug', default='two-sum')
    p.add_argument('--timeout', type=int, default=60)
    args = p.parse_args()
    base = args.base.rstrip('/')

    print(f'[demo] login as {args.user!r} on {base}')
    token = login(base, args.user, args.password)

    print(f'[demo] looking up problem_id for slug={args.problem_slug!r}')
    pid = get_problem_id(base, token, args.problem_slug)
    print(f'[demo] problem_id = {pid}')

    cases = [
        ('AC', CODE_AC),
        ('WA', CODE_WA),
        ('TLE', CODE_TLE),
    ]

    results = []
    for label, code in cases:
        print(f'[demo] === submit {label} (problem_id={pid}) ===')
        sid = submit(base, token, pid, 'cpp', code)
        print(f'[demo]   submission_id = {sid}; polling...')
        status, rt, mem = poll_until_terminal(base, token, sid,
                                              timeout_sec=args.timeout)
        print(f'[demo]   {label} → status={status} '
              f'runtime_ms={rt} memory_kb={mem}')
        results.append((label, sid, status, rt, mem))

    print('[demo] ───────── final state ─────────')
    print(f'[demo] {"expected":<10} {"submission_id":<14} '
          f'{"status":<10} {"runtime_ms":<10} {"memory_kb"}')
    for label, sid, status, rt, mem in results:
        ok = (label.lower() == status)
        print(f'[demo] {label:<10} {sid:<14} {status:<10} '
              f'{rt!s:<10} {mem!s} {"OK" if ok else "MISMATCH"}')

    bad = [r for r in results if r[0].lower() != r[2]]
    if bad:
        print(f'[demo] FAIL: {len(bad)} of {len(results)} did not match')
        return 1
    print('[demo] OK: all three terminals match expected labels')
    return 0


if __name__ == '__main__':
    sys.exit(main())
