#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# LiteCode-CPP — discussion image upload smoke test (v1.3.5 PR 13)
#
# 验证 POST /api/v1/discussions/upload-image 的安全与功能边界:
#   1. 无 JWT         → 401
#   2. 字段名不是 image → 400
#   3. 空文件         → 400
#   4. > 2MB          → 413
#   5. magic bytes 伪造 → 400
#   6. 合法 PNG       → 200 + .png URL + GET 字节一致
#   7. 合法 JPEG      → 200 + .jpg URL
#   8. 合法 GIF       → 200 + .gif URL
#   9. 合法 WEBP      → 200 + .webp URL
#  10. 两次上传 UUID 不同
#
# 用法(项目根目录):
#   python scripts/smoke_discussion_upload.py
#   LITECODE_BASE_URL=http://localhost:8080 python scripts/smoke_discussion_upload.py
#   LITECODE_USERNAME=tester LITECODE_PASSWORD=test123 python scripts/smoke_discussion_upload.py
#
# 依赖: 标准库 only(Python 3.8+)。无需 bcrypt / docker;只测 HTTP 公共端点。
# 退出码: 0 = 全部通过,非 0 = 失败计数值。
# 不修改任何数据库内容。

from __future__ import annotations

import io
import os
import struct
import sys
import urllib.error
import urllib.request
import json
import uuid
from typing import Callable, List, Tuple

BASE_URL = os.environ.get("LITECODE_BASE_URL", "http://localhost:8080").rstrip("/")
USERNAME = os.environ.get("LITECODE_USERNAME", "tester")
PASSWORD = os.environ.get("LITECODE_PASSWORD", "test123")

# ─────────────────────────────────────────────────────────────────────────
# 最小 1×1 像素图(纯字节,无第三方依赖)
# ─────────────────────────────────────────────────────────────────────────

# 1×1 白色 PNG(67 字节,IHDR/IDAT/IEND 完整)
PNG_1X1 = bytes.fromhex(
    "89504e470d0a1a0a"            # magic
    "0000000d49484452"            # IHDR length + tag
    "00000001000000010806000000"  # 1x1, 8-bit RGBA
    "1f15c489"                    # IHDR crc
    "0000000a49444154"            # IDAT length + tag
    "789c6300010000000500010d0a2db4"  # zlib stream
    "0000000049454e44ae426082"    # IEND
)

# 1×1 白 JPEG(SOF0 baseline;部分 marker 精简)
JPEG_1X1 = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000"
    "ffdb004300080606070605080707070909080a0c140d0c0b0b0c1912130f141d1a1f1e1d1a1c1c20242e2720222c231c1c2837292c30313434341f27393d38323c2e333432"
    "ffc0000b080001000101011100"
    "ffc4001f0000010501010101010100000000000000000102030405060708090a0b"
    "ffc4001f0100030101010101010101010000000000000102030405060708090a0b"
    "ffda0008010100003f00fb"
    "ffd9"
)

# 1×1 GIF87a
GIF87A_1X1 = (
    b"GIF87a"                       # header
    b"\x01\x00\x01\x00"             # 1×1
    b"\x80\x00\x00"                 # GCT flag, bg, aspect
    b"\xff\xff\xff"                 # white pixel
    b"\x00\x00\x00"                 # black pixel
    b"\x2c"                          # image descriptor
    b"\x00\x00\x00\x00"             # left, top
    b"\x01\x00\x01\x00"             # 1×1
    b"\x00"                          # no LCT
    b"\x02\x02\x44\x01\x00"          # LZW min code + data
    b"\x3b"                          # trailer
)

# 1×1 WEBP (VP8L lossless, 完整)
WEBP_1X1 = (
    b"RIFF" + struct.pack("<I", 20) + b"WEBPVP8L"
    b"\x0d\x00\x00\x00\x2f\x00\x00\x00\x00"
    b"\x00\x00\x00\x00\x00\x00\x00\x00"
)


def build_multipart(field_name: str, filename: str, content: bytes,
                    extra_fields: List[Tuple[str, str]] = None) -> Tuple[bytes, str]:
    """Build multipart/form-data body, returning (body, content_type_header)."""
    boundary = "----lc-smoke-" + uuid.uuid4().hex
    lines: List[bytes] = []
    for k, v in (extra_fields or []):
        lines.append(b'--' + boundary.encode())
        lines.append(f'Content-Disposition: form-data; name="{k}"'.encode())
        lines.append(b'')
        lines.append(v.encode())
    lines.append(b'--' + boundary.encode())
    lines.append(
        f'Content-Disposition: form-data; name="{field_name}"; filename="{filename}"'.encode())
    lines.append(b'Content-Type: application/octet-stream')
    lines.append(b'')
    lines.append(content)
    lines.append(b'--' + boundary.encode() + b'--')
    lines.append(b'')
    body = b'\r\n'.join(lines)
    return body, f'multipart/form-data; boundary={boundary}'


def http_request(path: str, method: str = "POST", body: bytes = None,
                 headers: dict = None) -> Tuple[int, dict, bytes]:
    req = urllib.request.Request(
        BASE_URL + path,
        data=body,
        method=method,
        headers=headers or {},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()
    except Exception as e:
        return -1, {}, str(e).encode()


# ─────────────────────────────────────────────────────────────────────────
# 登录拿 JWT
# ─────────────────────────────────────────────────────────────────────────

def login() -> str:
    body = json.dumps({"username": USERNAME, "password": PASSWORD}).encode()
    status, _, resp = http_request(
        "/api/v1/auth/login",
        method="POST",
        body=body,
        headers={"Content-Type": "application/json"},
    )
    if status != 200:
        raise RuntimeError(f"login failed: HTTP {status}; body={resp[:200]!r}")
    try:
        env = json.loads(resp)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"login parse error: {e}; body={resp[:200]!r}")
    return env["data"]["access_token"]


# ─────────────────────────────────────────────────────────────────────────
# Test cases
# ─────────────────────────────────────────────────────────────────────────

class TestRunner:
    def __init__(self) -> None:
        self.passed = 0
        self.failed: List[str] = []

    def case(self, name: str, fn: Callable[[], None]) -> None:
        try:
            fn()
            print(f"[PASS] {name}")
            self.passed += 1
        except AssertionError as e:
            print(f"[FAIL] {name}: {e}")
            self.failed.append(name)
        except Exception as e:
            print(f"[ERROR] {name}: {e}")
            self.failed.append(name)

    def report(self) -> int:
        total = self.passed + len(self.failed)
        print(f"\n=== {self.passed}/{total} passed ===")
        if self.failed:
            print("FAILED:")
            for n in self.failed:
                print(f"  - {n}")
            return 1
        return 0


def main() -> int:
    print(f"Base URL: {BASE_URL}")
    print(f"Login:    {USERNAME}")

    token = login()
    auth_headers = {"Authorization": "Bearer " + token}

    uploaded_urls: List[str] = []

    def t_no_auth():
        body, ctype = build_multipart("image", "t.png", PNG_1X1)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype},
        )
        assert status == 401, f"expected 401, got {status}: {resp[:200]!r}"

    def t_wrong_field_name():
        body, ctype = build_multipart("file", "t.png", PNG_1X1)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 400, f"expected 400, got {status}: {resp[:200]!r}"

    def t_empty_file():
        body, ctype = build_multipart("image", "empty.png", b"")
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 400, f"expected 400, got {status}: {resp[:200]!r}"

    def t_too_large():
        big = PNG_1X1 + b"\x00" * (2 * 1024 * 1024 + 1)
        body, ctype = build_multipart("image", "big.png", big)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 413, f"expected 413, got {status}: {resp[:200]!r}"

    def t_fake_magic():
        fake = b"<?xml version=\"1.0\"?><hack/>"   # 不是任何图片 magic
        body, ctype = build_multipart("image", "fake.png", fake)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 400, f"expected 400, got {status}: {resp[:200]!r}"

    def t_valid_png():
        body, ctype = build_multipart("image", "t.png", PNG_1X1)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 200, f"PNG expected 200, got {status}: {resp[:200]!r}"
        env = json.loads(resp)
        url = env["data"]["url"]
        assert url.endswith(".png"), f"URL 末尾应为 .png: {url}"
        uploaded_urls.append(url)
        # GET URL 字节对比
        status_get, _, resp_get = http_request(url, method="GET")
        assert status_get == 200, f"GET expected 200, got {status_get}"
        assert resp_get == PNG_1X1, f"GET 字节应与上传一致"

    def t_valid_jpeg():
        body, ctype = build_multipart("image", "t.jpg", JPEG_1X1)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 200, f"JPEG expected 200, got {status}: {resp[:200]!r}"
        url = json.loads(resp)["data"]["url"]
        assert url.endswith(".jpg"), f"URL 末尾应为 .jpg: {url}"
        uploaded_urls.append(url)

    def t_valid_gif():
        body, ctype = build_multipart("image", "t.gif", GIF87A_1X1)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 200, f"GIF expected 200, got {status}: {resp[:200]!r}"
        url = json.loads(resp)["data"]["url"]
        assert url.endswith(".gif"), f"URL 末尾应为 .gif: {url}"
        uploaded_urls.append(url)

    def t_valid_webp():
        body, ctype = build_multipart("image", "t.webp", WEBP_1X1)
        status, _, resp = http_request(
            "/api/v1/discussions/upload-image",
            body=body,
            headers={"Content-Type": ctype, **auth_headers},
        )
        assert status == 200, f"WEBP expected 200, got {status}: {resp[:200]!r}"
        url = json.loads(resp)["data"]["url"]
        assert url.endswith(".webp"), f"URL 末尾应为 .webp: {url}"
        uploaded_urls.append(url)

    def t_unique_uuids():
        # 两次上传 PNG,UUID 应该不同
        urls = []
        for _ in range(2):
            body, ctype = build_multipart("image", "t.png", PNG_1X1)
            status, _, resp = http_request(
                "/api/v1/discussions/upload-image",
                body=body,
                headers={"Content-Type": ctype, **auth_headers},
            )
            assert status == 200, f"upload failed: {status} {resp[:200]!r}"
            urls.append(json.loads(resp)["data"]["url"])
        assert urls[0] != urls[1], f"两次上传应得到不同 UUID: {urls}"
        uploaded_urls.extend(urls)

    runner = TestRunner()
    runner.case("无 JWT → 401",                       t_no_auth)
    runner.case("字段名不是 image → 400",             t_wrong_field_name)
    runner.case("空文件 → 400",                       t_empty_file)
    runner.case("> 2MB → 413",                        t_too_large)
    runner.case("magic bytes 伪造 → 400",             t_fake_magic)
    runner.case("合法 PNG → 200 + .png URL + 字节",    t_valid_png)
    runner.case("合法 JPEG → 200 + .jpg URL",          t_valid_jpeg)
    runner.case("合法 GIF → 200 + .gif URL",           t_valid_gif)
    runner.case("合法 WEBP → 200 + .webp URL",         t_valid_webp)
    runner.case("两次上传 UUID 不同",                  t_unique_uuids)

    if uploaded_urls:
        print(f"\nGenerated URLs (容器持久化在 /app/uploads/discussion_images):")
        for u in uploaded_urls:
            print(f"  {BASE_URL}{u}")

    return runner.report()


if __name__ == "__main__":
    sys.exit(main())
