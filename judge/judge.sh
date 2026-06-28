#!/usr/bin/env bash
# =============================================================
# LiteCode-CPP — judge.sh（占位版）
# =============================================================
# SPEC §7.1 / §7.2：判题执行脚本
# -------------------------------------------------------------
# 本文件由 web 服务通过 docker exec / docker run 注入参数调用。
# 当前为占位实现，Phase 4 任务会替换为完整流程：
#   1. 接收 JSON 任务描述（submission_id / 时间内存限制 / 测试点列表）
#   2. 编译（g++ + 安全标志，独立 10s 超时）
#   3. 运行（每个测试点独立运行，超时/超内存/OLE/RE 判定）
#   4. 输出 JSON 结果到 stdout，由 web 解析后写回 submissions 表
# =============================================================

# Phase 1 占位：
# - 如果收到 --help 或无参数：直接 exit 0（docker-compose 用 `command: ["true"]` 探活）
# - 如果被 web 调用并收到任务参数（JSON）：输出 SE 让 web 记录为系统错误
if [ "$#" -eq 0 ] || [ "$1" = "--help" ] || [ "$1" = "true" ]; then
    echo "judge.sh placeholder (Phase 4 will implement real judge logic)"
    exit 0
fi

# 收到任务但还没实现 → SE
echo '{"status":"se","error":"judge.sh not implemented (Phase 4)"}' >&2
exit 1