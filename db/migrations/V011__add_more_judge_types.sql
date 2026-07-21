-- =============================================================
-- V011__add_more_judge_types.sql
-- Extend test_cases.judge_type ENUM with ignore_case + ignore_all_whitespace (v1.3.1+)
--
-- Per SPEC §4.3 + §11 Phase 4 ☆ judge_type 扩展:
--   v1.2 (V003)   → exact / ignore_trailing / float_eps / special
--   v1.3.1 (this) → + ignore_case / ignore_all_whitespace
--
-- ignore_case            → 行级 cmp 前把所有字母 ASCII 归一化为大写（cmp -i 类）
--                          A-Z ⇄ a-z 不影响其它字符；非 ASCII 字节按原样 cmp
--                          不改 GBK / 拉丁扩展字母 / 中日韩（CJK 按原字节 cmp），
--                          与 Codeforces / AtCoder 主流 OJ 的 "case-insensitive"
--                          语义对齐
--
-- ignore_all_whitespace → 忽略所有 [ \t]+（不限尾部）+ 完全忽略空行
--                          比 ignore_trailing 严格：ignore_trailing 只 rstrip，
--                          提交写 "a b\n" 而期望 "ab\n" 仍会 PE；
--                          ignore_all_whitespace 把内部多空格折叠为单空格后 cmp，
--                          因此 "a  b\n" == "a b\n" == "a b\n"（前提是期望也归一化）
--
-- 注意：
--   1) 两个新值与现有 special / float_eps 在语义上**正交**（大小写 / 空白 vs 浮点容差 / SPJ），
--      未来 §4.3 可考虑把 ignore_case + ignore_all_whitespace 合并为 single normalize
--      选项（problem-level flag）来减少组合爆炸。本 commit 不做。
--   2) enum 末尾追加新值是无损操作（已有行的值不受影响）；MySQL 5.7+ / 8.0
--      的 ALTER COLUMN 在生产库上需要轻量 metadata lock（毫秒级），SPEC §15.6
--      的"无停机迁移"约束满足。
--   3) judge/lib/compare.sh 在 1.3.1 加 compare_ignore_case + compare_ignore_all_whitespace；
--      judge.sh step 3 case 分支新增 ignore_case / ignore_all_whitespace 两分支；
--      admin_bulk_import_routes.h + admin_problem_routes.h 的 validator 同步扩 2 值。
-- =============================================================

ALTER TABLE test_cases
    MODIFY COLUMN judge_type
        ENUM('exact','ignore_trailing','float_eps','special',
             'ignore_case','ignore_all_whitespace')
        NOT NULL DEFAULT 'exact';

INSERT INTO schema_migrations (version) VALUES ('V011');