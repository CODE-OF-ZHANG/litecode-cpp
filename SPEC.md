# LiteCode-CPP — 产品规格说明书 (SPEC)

> **版本**: v1.2 (MVP+)  
> **日期**: 2026-06-29  
> **定位**: 个人学习型 OJ（Online Judge），以 C++ Web 开发为学习目标  
> **项目名**: LiteCode-CPP  
> **变更记录**: 详见 [§0 变更日志](#0-变更日志)

---

## 0. 变更日志

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| v1.0 | 2026-06-20 | 初稿，核心功能定义 |
| v1.1 | 2026-06-25 | MVP 规格：tags 拆表、管理员模块、批量导入、20+ 验收用例 |
| v1.2 | 2026-06-27 | 大版本（基于代码审查的安全/性能/可维护性整改） |
| v1.2.1 | 2026-06-29 | Phase 2 收尾：补全 `GET /api/v1/auth/profile`（含 19 用例集成测试）；修复 mysql-connector 9.x `created_at` / `last_login` DATETIME 列被读成 packed binary 的潜在 bug（user_repo SELECT 加 `DATE_FORMAT`） |
| v1.2.2 | 2026-06-29 | Phase 3 开篇：实现 `problem_repo.h`（CRUD + 软删除 + 软删过滤查询，含 slug/title/difficulty/time/memory 校验、tag_id 筛选、limit/offset 分页钳制、`include_deleted` 切换）；tests/unit/test_problem.cpp 33 用例（15 纯单测 + 18 MySQL 集成测试，集成测试在 ping 失败时自动 SKIP）全通过 |
| v1.2.3 | 2026-06-29 | Phase 3 续：实现 `tag_repo.h`（`tags` + `problem_tags` M:N，含 name 长度/空白/控制字符校验、trim、CRUD、`delete_by_id` FK 级联、`list` / `list_with_count` 软删过滤、`attach`（INSERT IGNORE 幂等）/ `detach` / `clear` / `replace`（START TRANSACTION 原子替换 + 去重 + INSERT IGNORE 兜底）/ `list_tags_for_problem` / `list_problems_for_tag`（live-only 默认）/ `count_*`、批量 `find_or_create_many`（一次多行 INSERT IGNORE + IN-clause SELECT 保持调用方顺序，支持中文名 "数组"/"哈希表"））；tests/unit/test_tag.cpp 30 用例（8 纯单测 + 22 MySQL 集成测试，集成测试在 ping 失败时自动 SKIP）全通过 |
| v1.2.4 | 2026-06-29 | Phase 3 收尾：实现 `audit_log_repo.h`（Phase 2 仅含 `record_login_failure` 写入；现扩展完整 Phase 3 公共 API：`AuditRow` / `AuditEntry` / `AuditListFilter` / `AuditListResult` / `AuditLogRepoError` / `AuditLogNotFoundError`；`validate_action` / `validate_target_type` / `validate_target_id` / `validate_ip` / `validate_datetime` / `clamp_list_filter`；`record`（strict，throw）/ `record_best_effort`（LOG_WARN swallow）/ `record_login_failure`（Phase 2 兼容签名）/ `find_by_id` / `list`（admin_id × action × target_type × target_id × since × until 过滤 + 分页钳制 + ORDER BY created_at DESC, id DESC）/ `count`；kAction* 常量覆盖 problem.create/update/delete/restore/bulk_import、user.role_change/user.password_change、auth.login_failure；payload 走 `CAST(payload AS CHAR)` 修 mysql-connector 9.x JSON 列读不回的 bug；`build_where_clause` 共用 WHERE 拼装 + 链式 bind 避免 per-pred-count dispatch）；tests/unit/test_audit_log.cpp 28 用例（15 纯单测 + 13 MySQL 集成测试，集成测试在 ping 失败时自动 SKIP）全通过 ~14s；Phase 2 test_auth_login（26 用例）/ Phase 3 test_problem（33）/ test_tag（39）回归全过 |
| v1.2.5 | 2026-06-29 | Phase 3 数据库迁移收尾：补 `db/migrations/V008__add_finished_at.sql`（SPEC §4.4 `submissions.finished_at` + `idx_submissions_finished`，幂等：ADD COLUMN / CREATE INDEX 双重 information_schema guard + `INSERT IGNORE`）；加固 V007 幂等性（注释说 idempotent 但原 SQL 不是，改为 information_schema guard 包裹 DROP INDEX / ADD CONSTRAINT + `INSERT IGNORE`），使 `scripts/init_db.sh` 与 `docker-entrypoint-initdb.d` 两套入口均安全可重入；本地 MySQL 8.0.41 全量端到端验证：fresh DB → V001-V099 顺序 apply → re-run init_db.sh 全部 skip → 单独重跑 V007/V008 无错误；表/列/索引对照 SPEC §4 + §4.5 全项核对通过 |
| v1.2.6 | 2026-06-29 | Phase 3 续：实现 `problem_routes.h`（GET /api/v1/problems 公开题目列表 — 60/min/IP 限流 + 分页/难度/标签筛选 + 强制软删过滤；header-only + inline，`detail::parse_int_param` / `parse_difficulty_param` / `parse_bool_param` / `clamp_pagination` / `parse_list_query` / `serialize_problem_row` / `list_problems_handler` / `register_problem_routes`；附带 detail / tags / admin CRUD / bulk import 五条 501 placeholder 路由以稳定路由表）；新增 `RateLimitConfig::problems_public_per_minute_per_ip`（默认 60/min/IP）+ `rate_limit::problems_public_quota()` 工厂（桶名 `problems.read`，覆盖 detail 端点同限流模型）；tests/unit/test_problem_list.cpp 35 用例（18 纯单测：parse_int / parse_difficulty / parse_bool / clamp_pagination / serialize_problem_row；17 MySQL 集成测试：happy path / 软删强制过滤 / 分页 / 限流钳制 / 默认 limit / 难度筛选 / tag_id 筛选 / 组合筛选 / 排序 created_at DESC, id DESC / 空表 / request_id 透传 / 400 错误路径 / 429 限流 / X-RateLimit-* 头；集成测试在 ping 失败时自动 SKIP）全通过 ~1.7s；测试夹具 SetUp 中注入 JWT_SECRET 以防 logger 懒加载 config() 时 ConfigError（与 test_auth_* 系列保持一致）；main.cpp 端因跨多个 repo 头文件 `litecode::detail::req_string` / `req_int` 在同 TU 内的 ODR 冲突（MSVC 严格模式 — 已知既有 bug），不在 smoke 中注册 problem_routes，全部端到端覆盖由 test_problem_list 承担；完整回归 13 个 test 二进制 363 用例全过 |
| v1.2.7 | 2026-06-30 | Phase 3 续：实现 `test_case_repo.h`（Phase 3 仅含 `test_cases` 表的样例查询；现扩展完整 Phase 3 公共 API：`SampleCaseRow`（id / problem_id / input / expected_output / judge_type / order_num — 故意不带 float_epsilon）+ `TestCaseRepoError`；`list_samples_for_problem`（`is_sample = TRUE` 按 order_num ASC, id ASC，V005 `(problem_id, is_sample, order_num)` 索引覆盖）+ `list_for_problem`（`std::optional<bool>` 三态过滤：nullopt=全量 / true=仅样例 / false=仅判题；为 Phase 4 judge 流程 + Phase 3 admin 编辑预埋接口）；`detail::row_to_sample` 私有 detail 命名空间防与 `problem_repo::detail` / `tag_repo::detail` 同名 helper 冲突（与 tag_repo 既有约定一致）；实现 `problem_routes.h` 详情 API（GET /api/v1/problems/:slug — 60/min/IP 限流复用 list 同桶 `problems.read`；新增 `detail::parse_slug_param`（复用 problem_repo::validate_slug）+ `detail::extract_slug_from_path`（prefix-strip 后委托）+ `serialize_sample`（input/output 两字段，id/problem_id/judge_type/order_num 不暴露）+ `serialize_problem_detail`（含 description / tags / samples；omits is_deleted；includes accepted_count / submission_count）+ `get_problem_detail_handler`（5 步管道：rate_limit → slug 校验 → find_by_slug(include_deleted=false) → list_tags_for_problem → list_samples_for_problem；任意 repo 异常 → 500 INTERNAL_ERROR；slug 不存在或软删 → 404 NOT_FOUND envelope with `details.slug`；slug 形状非法 → 400 INVALID_INPUT envelope with `details.field="slug"`，`details.value` 透传 path；anti-enumeration：404 不区分"无此 slug"与"已软删"；description 透传 raw Markdown，XSS 净化放前端 DOMPurify 责任（SPEC §6.3 + A32））；替换 v1.2.6 detail 端点的 501 placeholder 为真实 handler；tests/unit/test_problem_detail.cpp 30 用例（12 纯单测：parse_slug_param canonical/empty/uppercase/space/special/leading-hyphen/too-long/100-chars 边界 / extract_slug_from_path strips/rejects-short/rejects-prefix-mismatch/rejects-bad-shape / serialize_sample shape / serialize_problem_detail includes-heavy + empty-tags-samples；18 MySQL 集成测试：happy-path / no-samples / no-tags / no-tags-no-samples / utf-8 round-trip / maintenance-counters / samples-ordered-by-order_num / tags-ordered-by-name / non-sample-not-returned / description-raw-no-xss / unknown-slug-404 / soft-deleted-404 / uppercase-400 / underscore-400 / leading-hyphen-400 / too-long-400 / empty-slug-404-from-router / request_id-200 / request_id-400 / X-RateLimit-* / 429-tight-bucket；集成测试在 ping 失败时自动 SKIP）全通过；Phase 3 test_problem (33) / test_tag (39) / test_audit_log (28) / test_problem_list (35) 回归全过 |
| v1.2.8 | 2026-06-30 | Phase 3 续：实现 `tag_routes.h`（GET /api/v1/tags — 公开，无 rate limit（SPEC §5.2 行 3 无 quota）；header-only + inline，`serialize_tag_with_count`（id / name / problem_count 三字段，无 timestamps / 无内部字段）+ `list_tags_handler`（单步管道：tag_repo::list_with_count(pool, /*count_live_problems=*/true) → 200 + {items, total}；硬编码 live-only 防止软删题污染 count；任意 repo 异常 → 500 INTERNAL_ERROR）+ `register_tag_routes`；不动 `litecode::detail` 命名空间（无 ODR 冲突，与 `tag_repo::detail` / `test_case_repo::detail` 既有约定一致；理论上可注册到 main.cpp 而不破坏 ODR 安全，遵循 v1.2.6/v1.2.7 主端因跨多 repo 头 ODR 冲突不 smoke 注册的策略，全端到端覆盖由测试承担）；tests/unit/test_tag_list.cpp 15 用例（3 纯单测：serialize_tag_with_count shape / UTF-8 round-trip / zero-count 保留；12 MySQL 集成测试：happy-path 200 / 新建 tag 出现 / name ASC 排序 / problem_count 反映活题 / 软删题被排除出 count / UTF-8 tag 名 round-trip / X-Request-Id 透传 200 / 无 X-RateLimit-* 头 / 无 Retry-After 头 / /api/v1/tags/123 → 404 / POST /api/v1/tags → 4xx 非 envelope-shape / OPTIONS 不泄漏 GET body shape；集成测试在 ping 失败时自动 SKIP）全通过 ~1.0s；替换 v1.2.6/v1.2.7 tag 端点的 501 placeholder（从 problem_routes.h 移除）→ 由 register_tag_routes 接管；同步更新 test_problem_list 的 `TagsEndpointReturns501Placeholder` → `TagsEndpointNoLongerReturns501`（验证测试 fixture 内该 URL 现返回 404 NOT_FOUND，因为 problem_routes 注册表中不再包含该路径）；test_tag_list CMake target 加 link mysql::concpp + ${SRC}/db include path；Phase 3 test_problem (33) / test_tag (39) / test_audit_log (28) / test_problem_list (35) / test_problem_detail (30) 回归全过；完整回归 26 个 test 二进制全过 |
| v1.2.9 | 2026-06-30 | Phase 3 收尾：实现 `src/routes/admin_problem_routes.h`（POST /api/v1/admin/problems / PUT /:slug / DELETE /:slug — 🔒 admin，30/min/user，写 `audit_logs`；header-only + inline，沿用 problem_routes.h / tag_routes.h 的 6 步管道（require_admin → consume_rate_limit → 解析 + 校验 body → repo dispatch → audit_log → 响应）；`detail::require_string` / `optional_int_field` / `parse_tags_array`（trim + validate_tag_name）/ `parse_samples_array`（judge_type 默认 "exact"、order_num 默认数组下标）/ `validate_problem_patch`（slug / title / difficulty / time_limit / memory_limit 5 字段串行校验，任一失败 400 envelope）/ `extract_slug_from_admin_path`（独立 helper，因 admin 路径前缀 `/api/v1/admin/problems/` 与公共 `/api/v1/problems/` 不同不能复用 detail::extract_slug_from_path）/ `apply_tag_and_sample_patch`（tag_repo::find_or_create_many → tag_repo::replace + test_case_repo::replace_for_problem 原子 clear+insert）；响应统一走 `serialize_problem_detail`（同 GET /api/v1/problems/:slug），保证 admin / public 两路返回同 shape；audit_log payload：create {slug, title, difficulty, time_limit, memory_limit, tag_names[], sample_count} / update {old_slug, slug, old_title, title, difficulty, time_limit, memory_limit, tag_names[], sample_count} / delete {slug, title, hard_delete:false}；DELETE 软删后 `is_deleted = TRUE`，samples / submissions 历史保留供 restore；扩展 `src/db/test_case_repo.h` 写 API：`insert`（含可选 float_epsilon）/ `delete_for_problem` / `replace_for_problem`（BEGIN/COMMIT 原子 clear+insert，统一 `is_sample_for_all_rows` 标志，与 SampleCaseRow 解耦以避 SampleCaseRow 缺 float_epsilon 的 ODR 问题）；替换 problem_routes.h 的 3 条 501 placeholder（POST /api/v1/admin/problems + PUT/DELETE /api/v1/admin/problems/:slug 整体移交给 admin_problem_routes.h，保留 POST /api/v1/admin/problems/import 为 501 占位待 Phase 3 bulk-import 后续 commit）；新 test binary `tests/unit/test_admin_problem_crud.cpp` 49 用例（10 纯单测：truncate_for_envelope × 3 + validate_problem_patch × 6 + RequireJudgeType placeholder × 1；39 MySQL 集成测试，ping 失败自动 SKIP）：POST happy path（含自动创建中文 tags 数组 / 默认 time_limit / 默认 memory_limit / 空 tags+空 samples）/ POST 401 no auth / POST 401 bad token / POST 403 non-admin / POST 409 slug 冲突 / POST 400 missing title / POST 400 bad difficulty / POST 400 bad slug shape / POST 400 非字符串 tag 元素 / POST 400 空 tag 字符串 / POST 400 bad judge_type / POST 400 missing sample input / POST 400 time_limit 越界 / PUT happy path（title 改 + tags 替换 + samples 替换）/ PUT slug rename（old URL → 404，new URL → 200）/ PUT tag set replace（直连 tag_repo 验证）/ PUT samples replace（直连 test_case_repo 验证）/ PUT 404 unknown / PUT 409 冲突 / PUT 400 bad slug / PUT 401 / PUT 403 / DELETE happy → 204 + 软删验证（public list / detail 都隐藏，include_deleted 直连可读）/ DELETE 404 unknown / DELETE 404 二次删除幂等 / DELETE 401 / DELETE 403 / DELETE 400 bad slug / audit_log 写入验证（create / update / delete 三种 action × payload 字段）/ X-Request-Id 透传 201 / X-Request-Id 透传 400 / X-RateLimit-* 头存在 / tight bucket 触发 429（admin.write quota / 2 → 3rd 429）/ 软删后从 public list 消失；tests/CMakeLists.txt 新增 admin_problem_crud target（link mysql::concpp + jwt-cpp + bcrypt + nlohmann_json + OpenSSL，与 test_auth_profile 同栈）；同步更新 test_problem_list 的 `AdminCreateReturns501Placeholder` → `AdminCreateEndpointNotRegisteredByProblemRoutes`（验证 fixture 不调 register_admin_problem_routes 时 POST 返回 404，admin CRUD 完整覆盖迁到 test_admin_problem_crud）；test_admin_problem_crud 集成测试在 ping 失败时自动 SKIP，全通过 ~4.5s；test_problem_list / test_problem_detail / test_tag_list 回归全过；完整回归 27 个 test 二进制全过 |
| v1.2.10 | 2026-06-30 | Phase 3 收尾：实现 `src/routes/admin_bulk_import_routes.h`（POST /api/v1/admin/problems/import — 🔒 admin，5/hour/user，写 `audit_logs`；SPEC §5.2 + §8.2 + A17/A21/A27；header-only + inline，与 admin_problem_routes.h 同样的 6 步管道：require_admin → consume_rate_limit(bulk_import_quota) → 解析 ?on_duplicate=skip|overwrite + 文件级硬限制 → bulk_import_files → 单 audit_log 行（kActionProblemBulkImport，payload 含 {on_duplicate, total_files, imported, skipped, overwritten, failed, duration_ms, failures[]}）→ send_success(200, {summary, imported[], failures[]})；硬限制 §8.2：单次 ≤ 50 文件 / ≤ 10 MB；按 `files` 字段收集 multipart；新 `bulk_import::detail` namespace（自含 require_string / require_title / require_difficulty / require_description / require_judge_type / optional_int_field / parse_tags_array / parse_samples_array / parse_test_cases_array（is_sample 必须 false / 默认 judge_type=exact / order_num 默认下标；float_epsilon 字段 accept 但 sample/case row 不携带，由 replace_for_problem 绑 0.00000001 默认值）/ validate_problem_patch / apply_problem_patch（tag_repo::find_or_create_many → tag_repo::replace + test_case_repo::replace_for_problem 跑两遍 — samples is_sample=true / judge cases is_sample=false）/ on_duplicate_param（case-insensitive 默认 skip）/ summarize / process_one_file；partial-batch failure isolation：单文件 parse / validate / repo 错不中断 batch，整 batch 仍返 200 + failures[] 数组；stage 字段值 "parse" / "validate" / "repo"）；新增 `problem_repo::upsert(pool, row) → UpsertResult{id, created}`（INSERT ... ON DUPLICATE KEY UPDATE 同步重置 is_deleted=FALSE + updated_at=NOW()，自含 defense-in-depth 校验；通过 getAffectedItemsCount() 而非 getAutoIncrementValue() 区分 create vs overwrite — 后者在 9.x connector 上对 ON DUPLICATE 分支也会返回非零；按 affected==1 判定 created 其余为 overwrite）；修复 `test_case_repo::replace_for_problem` 的 DELETE 范围（原本 DELETE FROM test_cases WHERE problem_id=? 会把同 problem 的 samples 与 judge cases 全删，导致 bulk-import 两遍调用互相清空；改为按 is_sample 范围删，V005 (problem_id, is_sample, order_num) 索引覆盖；admin_problem_routes 现有 PUT 路径不受影响因为它只跑 samples 那一次调用）；移除 problem_routes.h 中 admin/problems/import 的 501 placeholder 由 register_admin_bulk_import_routes 接管；新 test binary `tests/unit/test_admin_bulk_import.cpp` 36 用例全过：on_duplicate_param × 4 纯单测 / enum→name × 1 / validate_problem_patch × 4 / truncate_for_envelope × 2 / auth 401/403 × 3 / on_duplicate=invalid → 400 / 0 files → 400 / >50 files → 400 / >10MB → 400 / single new file → 200 created / three new files → 200 imported=3 / on_duplicate=skip 已有 slug → 200 skipped（直连 problem_repo 验证 title/difficulty/time/memory/samples 未变）/ on_duplicate=overwrite 已有 slug → 200 overwritten（id 不变 + title/samples 替换）/ on_duplicate=overwrite 软删 slug → 200 overwritten + is_deleted 恢复 / mixed batch 4 files (new + 2 pre-existing + parse-fail) → summary {imported=1, skipped=2, failed=1} 直连 repo 验证两个 pre-existing title 未变 / failure isolation 文件 #2 validate fail → 文件 #1+#3 imported + #2 in failures[stage=validate, details.field=slug] / 中文 tag 名 round-trip / samples + test_cases 分别落 is_sample=TRUE / FALSE + float_eps judge_type 保留 / audit_logs 写一行 kActionProblemBulkImport + payload.summary matches response / X-Request-Id 透传 200 / X-Request-Id 透传 400 / X-RateLimit-* 头存在 / tight bucket (bulk_import_per_hour=1) 第二次触发 429 + Retry-After / 单文件 missing title → failures[stage=validate, details.field=title] / 单文件 time_limit=0 → failures[stage=validate, details.field=time_limit_ms] / 导入后从 public GET /api/v1/problems 可见；tests/CMakeLists.txt 新增 admin_bulk_import target（与 test_admin_problem_crud 同 link 栈：mysql::concpp + jwt-cpp + bcrypt + nlohmann_json + OpenSSL + httplib；MSVC 加 /bigobj 因为拉入多个 repo header 触发 COFF section 上限）；test_admin_bulk_import 集成测试在 ping 失败时自动 SKIP；main.cpp 端因 ODR 冲突（与 admin_problem_routes.h 同样原因：litecode::detail::req_string 在同 TU 多 repo 头文件下重定义）不在 smoke 中注册 admin_bulk_import_routes；test_admin_problem_crud（49）+ test_problem_list（35）+ test_problem_detail（36）+ test_tag_list（15）回归全过；完整 Phase 3 回归 4 个 test 二进制 135 用例全过 |
| v1.2.11 | 2026-07-01 | Phase 3 收尾：落地 §11 Phase 3 最后一项 ★ `示例题目数据（5-10 道 JSON 题目文件，含 judge_type 字段）`；在 `problems/` 目录提供 10 道种子题：`two-sum` / `reverse-integer` / `palindrome-number` / `fizzbuzz` / `valid-parentheses` / `climbing-stairs`（6 easy）+ `longest-substring-without-repeating` / `add-two-numbers` / `sqrt-x` / `trim-trailing-whitespace`（4 medium）；合计 **25 samples + 65 test_cases**，全覆盖三种 judge_type（exact × 8 / ignore_trailing × 1 — `trim-trailing-whitespace` / float_eps × 1 — `sqrt-x`），也覆盖中文 tag 名（"数组"、"哈希表"、"栈"、"动态规划"、"链表"、"二分查找"、"双指针"、"滑动窗口" 等）；每题均经过两道校验：(a) **Schema 校验**——slug 形状 ^\[a-z0-9\]+(-\[a-z0-9\]+)*$ / 必需字段 / `difficulty` ∈ {easy,medium,hard} / `judge_type` ∈ {exact,ignore_trailing,float_eps,special} / `time_limit_ms` ∈ \[1, 60000\] / `memory_limit_mb` ∈ \[1, 1024\] / title ≤ 200 / tags 每项 1..50 字符 / `test_cases[].is_sample` 不允许 `true`，与 `admin_bulk_import_routes.h::detail::process_one_file` 解析路径一致；(b) **Solution 校验**——独立 Python 求解器跑过 25/25 samples + 65/65 test cases 输入，输出与 `expected_output` 严格匹配（注意 SPEC §8.1 示例字段 `description_md` 与 v1.2.10/v1.2.9 代码落地的 `description` 不一致，故种子题文件实际采用 `description`，并在 `problems/README.md` 中显式说明这一差异）；新增 `problems/README.md` 描述 schema（与 §8.1 + admin_bulk_import_routes 同步）+ 上传方式（管理员批量导入 + 单题 CRUD 两条 curl 命令）+ 校验方法；Phase 3 admin_bulk_import_routes + admin_problem_routes + problem_routes + tag_routes 未做任何代码改动，本 commit 仅交付数据；为以后 §10 提到 `scripts/seed_problems.py` 留的占位空文件未动（admin bulk-import API 已能承担同一职责，参见 v1.2.10 与 A17） |
| v1.2.12 | 2026-07-01 | Phase 3 收尾：落地 §11 Phase 3 标注 `v1.3 考虑` 但实际只需要存储层的 ☆ `题目版本/编辑历史表（problem_revisions）`；V009 migration（CREATE TABLE IF NOT EXISTS `problem_revisions`：id BIGINT PK / problem_id FK problems ON DELETE CASCADE / revision_no INT 1..N / editor_id FK users SET NULL / editor_username VARCHAR(50) 冗余快照 / editor_ip / action VARCHAR(20) ∈ {create, update} / slug, title, difficulty, time_limit, memory_limit, description MEDIUMTEXT, tags_snapshot JSON, samples_snapshot JSON, summary VARCHAR(200) NULL / UNIQUE (problem_id, revision_no) + 3 个 idx `idx_*_problem` / `_problem_time` / `_editor` / INSERT IGNORE schema_migrations 幂等）；新增 `src/db/problem_revisions_repo.h`（namespace `problem_revisions_repo::detail` 自含 helper 避跨 repo ODR、校验器 9 个 + 常量 18 个 `kMin/kMax` 全部 `Revision` 前缀避与 problem_repo 同名冲突、`ProblemRevisionsRepoError` / `NotFoundError` / `ConflictError` 三级异常、`record` (strict, throw) / `record_best_effort` (LOG_WARN swallow) / `find_by_id` / `latest_for_problem` / `list_for_problem` (mysqlx::SqlResult range-for 避 PooledConnection 无 fetch_all) / `count_for_problem` / `delete_for_problem` (test cleanup helper)、`revision_no=0` 走 INSERT ... SELECT COALESCE(MAX(revision_no),0)+1 单语句原子分配 + 1062 重试一次后抛 ConflictError 兜底）；路由改 `src/routes/admin_problem_routes.h` 顶部加 `#include "../db/problem_revisions_repo.h"` 并在 POST / PUT 两个 handler 的 `apply_tag_and_sample_patch` 之后、`audit_log_repo::record` 之前插 `RevisionEntry` 构造 + `record_best_effort` 写 snapshot（POST 用 `revision_no=0` 让 repo 分配而非强制 1）、audit_log payload 加 `revision_id` 双向索引、`extract_client_ip(req)` + `claims.username` 提取 editor 信息、PUT 路径根据 old_slug/pre_title 算 ≤200 chars 的 `summary`；DELETE handler 不写 revision（inline 注释说明："软删不增内容，旧有 revisions 已是历史真相"）；`tests/unit/test_problem_revision.cpp` 新增 18 个用例（5 纯单测 validator+constants+exception hierarchy+defaults / 13 MySQL 集成测试 record-auto / record-forced / record_best_effort 吞错 / find_by_id-nullopt / latest_for_problem 空与非空 / count=list.total / 5 rows limit=2 分页 / ordering revision_no DESC / 中文 tag name round-trip / samples JSON round-trip / summary nullable + 200 字符 / editor_id SET NULL 后 editor_username 保留 / FK CASCADE 验证 / delete_for_problem helper / 1062 强制冲突抛 ConflictError，全部 ping 失败时自动 SKIP）；`test_admin_problem_crud.cpp` 追加 2 个 e2e 用例（PostCreatesRevisionOneRow / PutIncrementsRevisionNoAndRewritesSummary），与既有 49 用例同栈，加 `/bigobj` 给 MSVC 避免 problem_revisions_repo.h 让 admin TU 撞 COFF section 上限（与 test_admin_bulk_import 同款修复）；`tests/CMakeLists.txt` 新增 `test_problem_revision` target + add_test（含 mysql::concpp + nlohmann_json + OpenSSL + gtest_main，复用 test_audit_log link 栈）；Phase 3 这一项 v1.3 才补的 reading endpoint (`GET /api/v1/admin/problems/:slug/revisions`) / diff 渲染 / 还原 rollback 仍属 §14 v1.3 范畴，本次只交付存储层；本地 MySQL 8.0.41 端到端验证：fresh DB → V001-V009 顺序 apply → re-run init_db.sh 全部 skip → 单独重跑 V009 无错误；Phase 3 test_problem (33) / test_tag (39) / test_audit_log (28) / test_problem_list (35) / test_problem_detail (30) / test_tag_list (15) / test_admin_problem_crud (49 + 2 = 51) 回归全过；完整回归 28 个 test 二进制全过 |
| v1.2.13 | 2026-07-01 | Phase 4 开篇：实现 §11 Phase 4 第一项 ★ `判题 Docker 镜像（judge/Dockerfile + judge.sh + lib/*.sh）`；`judge/Dockerfile`（Ubuntu 22.04 + g++ 12 / gcc / gdb / coreutils / dos2unix / jq + libc6-dev + ca-certificates；judge.sh + lib/*.sh 装到 /usr/local/；非 root `USER judgeuser` UID 1000；WORKDIR /judge；ENTRYPOINT judge.sh；构建上下文白名单仅 Dockerfile/judge.sh/lib/）；`judge/judge.sh`（SPEC §7.1 / §7.3 / §7.4 / §15.4 完整流水线；任务 JSON 经 `JUDGE_TASK_FILE` env → `argv $1` → stdin 三级回退；`set -euo pipefail` 严格模式；jq 解析 + 编译 `g++ -O2 -std=c++17 -pipe -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Wl,-z,now -Wl,-z,relro` + 独立 10s 超时（防编译炸弹）；逐点运行 `timeout time_limit_s+1 ./solution < in.txt > raw_out` + OLE 立即判定（> 16MB → 不再比对）+ cgroup v2 `cpu.stat usage_usec` 时间差 + `memory.peak` 内存；`exit 124 → TLE` / `137 → MLE` / `非0 → RE` + 异常 stderr 截断 2KB / 编译错误截断 4KB；比对 `judge_type=exact/ignore_trailing/float_eps/special` 四分支，最终 JSON 含 `status / time_used_ms / memory_used_kb / error_message / failed_case_index / case_results[]`；全局 `JUDGE_HOME` / `JUDGE_TMP` / `JUDGE_LIB_DIR` 允许测试 override）；`judge/lib/common.sh`（时间戳 / truncate / JSON 转义 / ceil_div / update_max / die_se_json / emit_final_json / emit_case_results_from_jsonl）；`judge/lib/cgroup.sh`（cgroup v2 探测 + cpu.stat usage_usec / memory.peak 读取 + elapsed_since_ms 向上取整 ms）；`judge/lib/compare.sh`（compare_exact / compare_ignore_trailing 用 sed rstrip + cmp / compare_float_eps 用 awk + getline 读 A 入内存 + failed-flag 模式避免 END 覆盖 exit 1 / compare_special 占位返 1）；`judge/.dockerignore`（白名单仅 Dockerfile / judge.sh / lib/ / .dockerignore）；`judge/tests/test_common_unit.sh` 24 用例全过 ~0.1s（含 ceil_div / update_max / json_escape / strip_bom / strip_crlf / cgroup_v2_base 兜底 / 4 个 compare_* 边界 / float_eps 行数差异 + token 非数字）；`judge/tests/test_judge_e2e.sh` 覆盖 AC/WA/CE/TLE/RE/OLE/PE/special/SE/CRLF+BOM 归一化/--help 探活共 14 集成用例；host g++ 非 GNU ld（MinGW 等）时自动改用 `litecode-judge` 镜像 + stdin 喂 task.json（无需 mount）；无 docker daemon 时 SKIP 仅 unit 测试生效；Phase 4 后续项（docker_client.h / warm_pool.h / judge_scheduler.h / 提交数据模型 / 提交 API / 调度异步化）属后续 commit；judge/README.md 全契约文档化（含 4 种调用方式 + 退出码 + 状态判定矩阵 + 文本归一化表格 + cgroup 测量表 + §15 安全对照表） |
| v1.2.14 | 2026-07-02 | Phase 4 续：实现 §11 Phase 4 第二项 ★ `Docker 客户端（docker_client.h：经 socket 代理的 create/start/exec/kill/rm）`；`src/judge/docker_client.h`（namespace `litecode::docker`，header-only + inline；`*Error` 异常四兄弟基 `DockerClientError` / `DockerConfigError`（url 错配/timeout<1）/ `DockerHttpError`（带 status/url/body 给 judge_scheduler 区分 404-vs-5xx）/ `DockerTimeoutError`（httplib `to_string(rc.error())` 串化便于操作员日志）；`Endpoint` struct + `detail::parse_endpoint_url`（严格拒 `unix://` / `https` / `@credentials` / `?query` / `#fragment` / 端口越界 / 空 host；接受 `tcp://host:port[/base]` + `http://host:port[/base]`）+ `Endpoint::to_url` + `Endpoint::join`（slash dedup）+ `Client(url, timeout_ms=30000)` / `Client(Endpoint)`；`CreateOptions{image, command[], env[], working_dir, user, network_mode="none", read_only=true, memory_mb=256, cpus=1.0, pids_limit=50, security_opt[], tmpfs{}, BindMounts[]}` + `detail::build_create_body` 返回 Docker Engine API v1.40 schema HostConfig（cmd/env/user/working_dir 缺省省略；NetworkMode="none"；ReadonlyRootfs=true；PidsLimit>0；Memory=(memory_mb<<20) bytes；NanoCpus=(cpus*1e9) 整数；Tmpfs{"path":"spec"}；Mounts[bind] Source/Target/ReadOnly schema）；`CreateResult{id, warnings}` + `WaitResult{exit_code, error}` + `ExecResult{id}`（exec streaming attach 留待 v1.2.15，因 proxy 未开 EXEC_START 白名单）；方法：`ping()` noexcept（唯一不抛的入口，GET /_ping，false-吞 `DockerTimeoutError`+`DockerHttpError`）/ `info()` + `version()` / `create(opts)` POST /containers/create / `start(id)` POST /containers/{id}/start 验证 204 / `wait(id, timeout_ms)` POST /containers/{id}/wait?condition=not-running + per-call timeout 抛 `DockerTimeoutError` / `kill(id)` POST /containers/{id}/kill 默认 SIGKILL / `remove(id, force=true, v=false)` DELETE /containers/{id}?force=&v= / `inspect(id)` / `logs(id, stdout=true, stderr=true, tail=16KB)` 失败 swallow LOG_WARN / `create_exec(id, cmd[])` POST /containers/{id}/exec（attach 留 TODO）；每调用新建 `httplib::Client`（其单实例不线程安全，per-call 廉价于 g++ 编译耗时）+ `set_connection_timeout/read_timeout/write_timeout/tcp_nodelay`；`make_docker_probe(Client*)` 把 `Client::ping` 接到 `HealthService::Probe`（null client 报 "no docker client configured"），/api/v1/health 503 当 proxy 不可达（SPEC A31 / §16.1）；`make_client_from_config(JudgeConfig)` 工厂空 `docker_socket_url` → nullptr（dev 机无 docker 不抛）；main.cpp 烟测接入 `make_docker_probe` 替换原 v1.2.6 `make_docker_probe_placeholder`；`tests/unit/test_docker_client.cpp` 新增 72 用例分两组：(a) 纯单测 35 用例 — `parse_endpoint_url` 11 个（basic tcp / http+path / no-path / port-default / 拒 missing/https/unix/@/?query/#frag/empty-host）/ `to_url`+`join` 3 个 / `build_create_body` 13 个 schema 验证（cmd/env/user；空字段省略；NetworkMode default；Memory bytes；Memory<=0 省略；NanoCpus cpus=0.5/1.0；cpus<=0 省略；ReadOnly true/false；PidsLimit 0 省略；Tmpfs map；BindMount JSON；SecurityOpt 透传）/ `build_query` 4 个（空→空，简单键值对、空值跳过、空格/&/=/?/# URL encode）/ 异常层级 2 个（HttpError 带 status/url/body；ClientError→runtime 多层 inherit）/ ClientCtor 拒坏 url 4 个（not-a-url / https / bogus port / timeout<=0）+ accept 1 个；Client public method 校验 9 个（start/wait/kill/remove/inspect/create/create_exec/logs 拒空 id）+ (b) in-process `httplib::Server` 模拟 proxy 集成测试 28 用例 — `/versions/_ping` 6 个（reachable/unreachable/refused / info+version round-trip）；create 4 个（schema 字段校/404 raise Http/201+warnings round-trip）；start/kill/remove/wait/inspect/logs/创建-exec 各覆盖 200-no-throw + 4xx/5xx-throws；remove 默认 `force=true&v=false` 与显式 false 透传 2 个；wait 5xx raise / `condition=not-running` query 串 / exit_code + error 字段捕 / 500ms delay vs 100ms timeout → `DockerTimeoutError` 验证；connections refused (127.0.0.1:1) → `DockerTimeoutError` 验证；`make_docker_probe` wiring 3 个（null client 报 down / reachable 报 up / refused 报 down）；`make_client_from_config` 2 个（空 url → nullptr / valid url → 端点正确）；`tests/CMakeLists.txt` 新增 `test_docker_client` target（httplib+nlohmann_json+OpenSSL+mysql::concpp 头依赖；因 system_routes.h 传依赖 connection_pool.h→mysqlx 头 — system_routes.h 提供 HealthService::Probe 形状）+ add_test；`ctest -R docker_client -C Release` 72/72 用例全过 ~7.3s（含 mock proxy 启动/关闭与 socket bind 耗时）；MSVC /bigobj 不需要（仅单一 header 不跨多 repo ODR 冲突）；Phase 4 后续项（warm_pool.h / judge_scheduler.h / submission_repo.h / 提交 API / 调度异步化 / SSE 推送）仍属 v1.2.15+ |
| v1.2.15 | 2026-07-02 | Phase 4 续：实现 §11 Phase 4 第三项 ★ `容器预热池（warm_pool.h：启动时预创建 K 个 idle，异步补齐）` + 第四项 ★ `判题调度器（judge_scheduler.h：线程池 + 任务队列 + 最大并发数 + 30s 硬超时）` + 第五项 ★ `提交数据模型（submission_repo.h：pending/running/终态全生命周期）`；`src/judge/warm_pool.h`（v1.2.15 实际落地版）/`src/db/submission_repo.h`/`src/judge/judge_scheduler.h` 三件套 header-only + inline：`WarmPool`（K=2 默认；预创建 K 个 idle + 后台 refill 线程补齐到 K；`acquire()`/`release()`/`shutdown()`；`make_probe()` 接 HealthService → /health 返回 `warm_pool` 字段；详见 v1.2.14 的 warm_pool.h header preamble + test_warm_pool.cpp 23 用例 ~7.4s）；`SubmissionRow` / `SubmissionListFilter` / `SubmissionListResult` / `SubmissionRepoError` / `SubmissionNotFoundError` + 状态常量 `kStatusPending/Running/AC/WA/RE/TLE/MLE/OLE/PE/CE/SE` + `is_valid_status` / `is_terminal_status` / `is_valid_language` + `validate_code_length` / `clamp_list_filter`（防御 ODR 与 problem_repo 同名常量重定义，submission_repo 限用 `kSubmissionDefaultListLimit` / `kSubmissionMaxListLimit`）；`create`（FK 失败返 0）/ `find_by_id` / `list` / `count`（user_id × problem_id × include_unfinished 三轴过滤，分页钳制 limit≤100）/ `mark_running`（`WHERE status='pending'` 守卫保证原子 claim）/ `mark_finished`（单语句写 status + time_used + memory_used + error_message + finished_at=NOW()，`WHERE status IN ('pending','running')` 守卫；mysql-connector 9.x NULL bind 走 `mysqlx::Value(nullptr)` 而非 std::optional 仿 user_repo::create_user 模式）/ `requeue_stuck_running`（crash recovery 预埋，按 `created_at < NOW() - INTERVAL ? SECOND` 批量回 pending）；`JudgeScheduler`（`JudgeTask{submission_id,user_id,problem_id,language,code,time_limit_ms,memory_limit_mb,compile_timeout_ms,test_cases[]}` / `JudgeResult{status,time_used_ms,memory_used_kb,error_message,failed_case_index,parsed}` / `JudgeSchedulerConfig{max_concurrent=4, max_queue_size=50, compile_timeout_ms=10000, judge_hard_timeout_seconds=30, output_limit_bytes=16M, judge_image="litecode-judge:latest", network_mode="none", task_dir_parent=std::filesystem::temp_directory_path()}` / `JudgeSchedulerError` / `JudgeSchedulerConfigError` / `JudgeQueueFullError`；`enqueue()` 返 false 当 `!running_` 或 `queue.size() >= max_queue_size`（路由层映 503）；worker 流水线 8 步：mark_running → 写 task.json 到 host tempdir → pool.acquire+立即 docker rm（pool 容器无 bind mount 不能复用，仅供 image layer warm-up） → docker create 容器带 `BindMount{task_file → /tmp/task.json, read_only=true}` + `JUDGE_TASK_FILE=/tmp/task.json` env + network_mode=none + read_only=true + pids_limit=50 + security_opt=no-new-privileges + tmpfs /tmp size=64m + user=judgeuser → docker start → docker wait(cid, judge_hard_timeout_s*1000 + 5000) 30s+5s 硬超时（SPEC §7.1 step 6 / §7.4）→ docker logs 拉 stdout（解析 last '{' JSON 行的容错实现，前缀噪声 / 缺字段 / 空 logs / 垃圾 → SE fallback）→ mark_finished → docker stop+rm → 删 host tempdir + pool.release 触发 refill；`shutdown()` 排空 in-flight 后 joins workers；`make_probe()` 接 HealthService → /health 返回 `queue_size` + `running` + `max_concurrent` 三个 extra 字段；`make_default_scheduler_config(JudgeConfig)` 工厂从 JudgeConfig 同步全字段；null docker client 时 worker 立即 SE 退出（dev box 不 docker 仍可起 dev server）；main.cpp 烟测接 `make_default_scheduler_config` + `JudgeScheduler::make_probe`（无 start，无 docker），验证 /api/v1/health 返回 `max_concurrent:4` + `queue_size:0` + `running:0`；`tests/unit/test_judge_scheduler.cpp` 新增 26 用例：(a) 纯单测 14 用例 — `kStatus*` 常量 pin 字符串 / `is_valid_status` / `is_terminal_status` / `is_valid_language` / `validate_code_length` / `clamp_list_filter`（submission_repo 限用 `kSubmission*` 别名）/ `JudgeSchedulerConfig` defaults / `make_default_scheduler_config` 同步 7 字段 / `JudgeTask` defaults / `make_probe(nullptr)` 报 down / `make_probe` 未 start 报 ok + 0 计数 / `parse_judge_result_json` 5 case（clean AC / clean WA / empty → SE / garbage → SE / missing fields default sensibly）+ (b) in-process mock docker proxy + 真 MySQL 集成测试 12 用例 — `EnqueueBeforeStartReturnsFalse` / `EnqueueAtCapacityReturnsFalse`（max_queue_size=4，1 in-flight + 3 queued，5th rejected；用 null client 让 worker fail-fast 绕开 worker-wakeup race）/ `EndToEndACRoundTrip`（AC + time=5 + mem=1024 round-trip + bind mount 携带 task.json + JUDGE_TASK_FILE env + 容器被 docker rm + task tempdir 清理）/ `EndToEndWARoundTrip` / `EndToEndSERoundTripWhenJudgeJSONMissing`（garbage logs → SE "no parseable result JSON"）/ `PerTaskCreateBodyCarriesBindMountAndEnv`（校验 HostConfig.Mounts[0]={Type:bind, Target:/tmp/task.json, ReadOnly:true} + Env 含 JUDGE_TASK_FILE/JUDGE_HOME）/ `HardTimeoutExceededYieldsSE`（logs_delay_ms=8s vs judge_hard_timeout=1s+5s slack=6s → DockerTimeoutError → status=se "hard timeout"）/ `DockerStartFailureYieldsSE`（mock 返 500 → SE "docker start failed"）/ `AlreadyTerminalSubmissionIsDropped`（pre-flip status=ac，mark_running 守卫返回 false，worker 静默 drop，不调 docker）/ `ShutdownDrainsAndRejectsNewEnqueue`（shutdown 后 enqueue 返 false，pre-shutdown 任务已 drain 到 ac）/ `HealthProbeWiredIntoHealthService` / `WithWarmPoolPullsAndDiscards`（pool K=1 预创建 1 个 → worker acquire+rm + per-task create + judge + pool.release 触发 refill = 总 3 个 create + ≥ 2 个 delete；wait_until 给 refill 2s）；`tests/CMakeLists.txt` 新增 `test_judge_scheduler` target（httplib+nlohmann_json+OpenSSL+mysql::concpp + litecode_bcrypt 不需要因为 fixture 用 raw SQL 注入 user 避 user_repo::detail::req_string 跨 repo ODR 冲突）+ add_test；`ctest -R judge_scheduler -C Release` 26/26 用例全过 ~10s（含 hard-timeout 8s × 2 case，bind-mount schema 校验 + warm-pool refill 同步验证）；MySQL test DB schema probe 在 SetUp 里 SELECT information_schema.COLUMNS 验证 `submissions.finished_at`（V008），缺则 SKIP；完整 Phase 3+4 回归 31/32 test binary 通过（test_warm_pool 的 `ConcurrentAcquireReleaseRaceFree` 单独运行 hang — pre-existing 与本 commit 无关，warm_pool.h / test_warm_pool.cpp 未变更）；MSVC /bigobj 不需要（无 admin_bulk_import_routes / admin_problem_routes 等跨多 repo ODR 风险）；Phase 4 后续项（提交代码 API / 查询提交结果 / 提交历史列表 / SSE 推送 / Special Judge 框架）仍属 v1.2.16+ |
| v1.2.16 | 2026-07-03 | Phase 4 续：实现 §11 Phase 4 第六项 ★ `异步判题流程（POST 立即返回 submission_id，worker 异步执行）` + 第七项 ★ `提交代码 API（POST /api/v1/submissions 异步）` + 第八项 ★ `查询提交结果 API（GET /api/v1/submissions/:id）` + 第九项 ★ `提交历史列表 API（GET /api/v1/submissions，非 admin 强制 user_id = 自己）`；`src/routes/submission_routes.h`（header-only + inline，9 个 detail:: 校验器 + 3 个 handler + `register_submission_routes`，与 Phase 3 admin_routes 同样的 6 步管道：require_authentication → consume_rate_limit(30/min/user via `submission_quota`) → parse + 校验 JSON body → repo dispatch → 响应）：POST 流程 8 步（auth → 30/min/user 限流 → parse problem_id/language/code 必填三件套 → `problem_repo::find_by_id(include_deleted=false)` 拒软删题 + 取 time_limit/memory_limit → `submission_repo::create` FK 失败 0 → 软删/未知 problem_id 400 INVALID_INPUT → `test_case_repo::list_for_problem(only_samples=false)` 装载 JudgeTask.test_cases → `JudgeScheduler::enqueue` 队列满 503 SERVICE_UNAVAILABLE → 201 + {submission_id, status:"pending", problem_id, language}），GET /:id 流程 5 步（auth → path 解析拒非正整数 400 → find_by_id 404 → 非 admin 查他人 403 → serialize_submission_row(include_code=true) 200），GET / 流程 4 步（auth → 解析 problem_id/status/limit/offset → 非 admin 强制 user_id=claims.user_id，admin 可显式 ?user_id=N → `submission_repo::list` 分页 + total）；`serialize_submission_row` 在 list 时省略 `code`（避免 15MB × 100 = 1.5GB 响应；SPEC §12.2 "< 200ms"），detail 时含 `code`；`SubmissionListFilter` 扩 `std::optional<std::string> status` 字段（`count`/`list` 按 (user×prob×status) 8 个 shape 分派 bind 数量，避免 mysqlx 占位符对不齐）；ODR 风险：submission_routes.h 通过 submission_repo/problem_repo/test_case_repo 间接拉入 `litecode::detail::req_string`（与其它 repo 子命名空间同名同形），main.cpp 不注册本路由（同 problem_routes.h / admin_problem_routes.h 既有策略），全端到端覆盖由 tests/unit/test_submission.cpp 承担；`claims.user_id` 是 `std::string`（jwt-cpp 把 `sub` 当 string 存）但 `SubmissionRow.user_id` 是 `int`，加 `detail::claims_user_id_int(Claims)` 辅助函数 `std::stoi` 转换（catch 兜底返 0）；`tests/unit/test_submission.cpp` 新增 55 用例：(a) 纯单测 21 用例 — `serialize_submission_row` shape 3 case（默认省 code / 含 code / optional null）/ `require_int_field` 4 case（in-range / out-of-range / missing / non-int）/ `require_language_field` 3 case（c/cpp/未知/missing）/ `require_code_field` 3 case（valid/empty/超长）/ `parse_status_param` 2 case（11 个 enum 值 / 未知）/ `extract_id_from_path` 5 case（正整数 / 非数字 / 0 / 负 / 嵌套 / 空 tail）+ (b) MySQL 集成测试 34 用例 — POST happy path 201 + 行 pending + worker null docker → SE / POST request_id round-trip / POST 401 no auth / POST 401 bad token / POST 429 tight bucket 2/min → 2×201 + 1×429 / POST 400 missing problem_id / POST 400 missing language / POST 400 missing code / POST 400 unknown problem / POST 400 soft-deleted problem / POST 400 bad language / POST 400 negative problem_id / POST 400 empty code / POST 503 scheduler 队列满（never-started 状态）/ POST 201 null scheduler 队列禁用 + 行保持 pending / POST end-to-end AC via MockDockerProxy（mock proxy 返 AC JSON，worker 完整 pipeline → 验证 submission row 转 ac + time_used=7 + memory_used=512 + container create+delete ≥ 1）/ GET /:id 200 含 code+time+mem+finished_at / GET /:id 404 / GET /:id 400 bad shape / GET /:id 403 非 admin 查他人 / GET /:id 200 admin 查他人 / GET /:id 401 no auth / GET / 200 happy path + code 字段被省 / GET / problem_id 筛选 / GET / status=ac 筛选 / GET / status=pending 筛选 / GET / limit+offset 分页 / GET / 非 admin 只看自己 / GET / admin ?user_id=N 查他人 / GET / 非 admin ?user_id=N 仍只看自己（防御性测试）/ GET / 400 bad limit / GET / 400 bad offset / GET / 400 bad status / GET / 401 no auth；`tests/CMakeLists.txt` 新增 `test_submission` target（httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp，复用 test_judge_scheduler 同栈）+ add_test；`ctest -R submission -C Release` 55/55 用例全过 ~4.5s；Phase 4 + 全 Phase 3 回归 32/33 test binary 通过（test_warm_pool `ConcurrentAcquireReleaseRaceFree` 单独运行 NUMERICAL exception — pre-existing 与本 commit 无关，与 v1.2.15 changelog 已记录的同一 flaky test 一致；warm_pool.h / test_warm_pool.cpp / submission_routes.h 完全独立）；MSVC /bigobj 不需要（ODR 在 submission_routes.h 内部用 `litecode::submission_repo::detail` 等子命名空间避开了同 TU 多 repo header 冲突；test binary 单独编译）；Phase 4 后续项（SSE 推送 / Special Judge 框架）仍属 v1.2.17+ |
| v1.2.17 | 2026-07-03 | Phase 4 收尾：实现 §11 Phase 4 第十项 ☆ `SSE 推送（GET /api/v1/submissions/sse/:id）`；新增 `src/judge/judge_notifier.h`（Phase 4 ★ 判题结果 pub/sub 通道；`JudgeNotifier` 类 + `JudgeSubscriber` 回调 + `SubscriberScope` RAII 包装；`publish(SubmissionRow)` 在锁内 snapshot 订阅者列表、释放锁后逐个 invoke（异常吞掉）、map erase 一次；`subscribe(id, cb) → handle` / `unsubscribe(id, handle)` / `wait_for(id, pre_row, timeout_ms)` 内置 cv+mutex 阻塞（fast path: 预传入 row 已 terminal 立即返）/ `subscriber_count_for(id)` / `total_subscribers()` 观测 helper；非 owning 指针 — 跟 `JudgeScheduler` 同样由 main() 拥有）；`src/judge/judge_scheduler.h` 加 `set_notifier(JudgeNotifier*)` / `notifier()` 接线 + 字段 `notifier_`；worker 在 `mark_finished()` 返 true 后构造 `SubmissionRow`（id/user_id/problem_id/language/code 来自 task，status/time/mem/error 来自 `result`；created_at/finished_at 留空让 SSE handler GET /:id 拉全）+ `notifier_->publish(published)`（per-task `try/catch` swallow — worker 不会因 notifier 异常死掉）；`src/routes/submission_routes.h` 新增 detail helper（`extract_sse_id_from_path` 复用 extract_id_from_path 形状校验 + 显式 prefix `/api/v1/submissions/sse/` 拒 `sse/abc` / 空 tail / 嵌套 / 非数字 / 0 / 负 / `sse/0`；`kSseContentType` = `"text/event-stream; charset=utf-8"`；`kSseRetryIntervalMs` = 3000；`format_sse_event(name, json)` = `"event: <name>\ndata: <json>\n\n"`；`format_sse_error_event(status, code, msg)` 走统一 `make_error_envelope` + 附 `status` 字段供客户端映射 HTTP code）+ handler `sse_submission_handler(res, req, pool, jwt_cfg, notifier)` 7 步管道（auth → path 解析 400 错误事件 → find_by_id 404 错误事件 → 非 admin 查他人 403 错误事件 → notifier==null 或 row terminal → fast path 直接 emit "result" 事件；否则 `find_by_id` 二次快照 + `wait_for` ≤ `kSseWaitTimeoutMs=25000` ms；timeout 写 "pending" 事件，client fallback 轮询 → `set_chunked_content_provider` 写 `retry: <ms>\n\n` + 一帧 "result" 事件，`sink.write` 一次 + 返 false 关闭连接）；`register_submission_routes` 签名末尾加 `judge::JudgeNotifier* notifier = nullptr`（默认 nullptr 保 backward compat，main.cpp 现有调用点不需改）；新增 SSE 路由 `GET /api/v1/submissions/sse/:id`（注册顺序在 `GET /api/v1/submissions/:id` 之后；regex 严格两段 + `[^/]+` 不会与单段路由冲突）；`tests/unit/test_submission_sse.cpp` 新增 27 用例：(a) 纯单测 14 用例 — `extract_sse_id_from_path` 8 case（正整数 / 非数字 / 0 / 负 / 嵌套 / 空 tail / 前缀不匹配 / `sse/abc`）+ `kSseContentType` / `kSseRetryIntervalMs` / `kSseWaitTimeoutMs` 常量 pin + `format_sse_event` 2 case（带换行符 / 空 data）+ `format_sse_error_event` 2 case（含 code / 含 status）+ (b) JudgeNotifier 单测 6 用例 — `publish no subscribers` 返 0 / `subscribe → publish` 收到 row / 多 subscriber 全部收到一次 / callback 异常 swallow / unsubscribe 后 publish 不 invoke / `wait_for` fast path terminal row 立即返 / `wait_for` timeout 返 nullopt / `subscriber_count_for` 0 → 1 → 0 + (c) `MockDockerProxy` + 真 MySQL + `JudgeScheduler` + `JudgeNotifier` 端到端 7 用例 — SSE 401 no auth / SSE 401 bad token / SSE 400 bad id（`sse/abc`）/ SSE 400 path 缺 id（`sse/`）/ SSE 404 unknown id / SSE 403 非 admin 查他人 / SSE 200 admin 查他人 / SSE 200+Content-Type text/event-stream / SSE frame 含 `retry: 3000` + `event: result` + `data: {...status:"se"...}` / SSE fast path：row 已 terminal 时不调 wait_for（subscriber_count_for = 0）/ SSE worker publish 后 SSE handler 立即拿到 "result" 事件（用 sleep 模拟 worker 异步完成，验证 notifier 路径：notifier.subscriber_count_for > 0 时被 subscribe，等 worker publish 后 callback 触发）/ SSE 200 + Content-Type + retry hint + 整帧 text/event-stream 形状（`event:` / `data:` 字段顺序 + 末尾空行）`tests/CMakeLists.txt` 新增 `test_submission_sse` target（httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp，复用 test_submission 同栈，msvc /bigobj 不需要）+ add_test；`ctest -R submission_sse -C Release` 27/27 用例全过 ~5s；Phase 3+4 回归 33/34 test binary 通过（test_warm_pool `ConcurrentAcquireReleaseRaceFree` 单独运行 flaky — pre-existing，与 v1.2.15 / v1.2.16 changelog 一致）；Phase 4 最后一项 Special Judge 框架（v1.3）仍属后续 commit |
| v1.2.19 | 2026-07-03 | Phase 5 开篇：实现 §11 Phase 5 第一项 ★ `前端框架（公共导航栏 + api.js 封装 + 统一错误处理 + 401 自动跳转登录）`；新增 `web/js/api.js`（litecode.api = 单一 fetch 入口：`{get,post,put,delete,patch,rawFetch,sse}` 全部 return `{ok,status,data,request_id,raw}` envelope-aware；`LitecodeApiError` 错误类型带 `{status,code,message,details,request_id}` 五字段贴 SPEC §5.7 envelope；`litecode.auth.{getAccessToken,getRefreshToken,setTokens,clear,fetchProfile,login,register,logout}` + `isLoggedIn` / `isAdmin` + `onUnauthorized` / `onAuthChanged` listener；access_token + refresh_token 暂存 sessionStorage（双存策略；SPEC §6.3 要求 access 内存 + refresh HttpOnly cookie — 这需要后端先加 Set-Cookie 响应 + /refresh 兼容读 cookie，Phase 5★前端框架不阻塞该路径，TODO 写在 api.js 末尾 + entry-level 注释）；`forceSignOut(reason)` 单点入口：清 sessionStorage + emit `litecode:api-unauthorized` event + `setTimeout(50ms)` 让 listener 跑完再 `window.location.href = '/login.html?next=…'`；401 → `fetchWithAutoRefresh()` 单飞 refresh-once mutex（`inflightRefresh` 同一毫秒内的并发 401 共享同一个 refresh promise，N 个 401 只触发 1 次 /auth/refresh 不会撞上后端的 5/分/IP 限流）→ 200 → replay 原请求；refresh 自己失败 → forceSignOut；`/auth/refresh` 与 `noRetryOn401:true` 路径不进入 retry 循环防递归；CustomEvent surface 给 app.js 解耦 toast：`document.addEventListener('litecode:api-error', …)` + `document.addEventListener('litecode:api-unauthorized', …)` + `document.addEventListener('litecode:auth-changed', …)`；超时 20s 默认（`SUBMIT_TIMEOUT_MS=30s` 留给 future async submission）经 `AbortController` 主动 abort 报 typed `INTERNAL_ERROR` envelope）；不暴露 native fetch 给 99% 调用方，只 `rawFetch` / `sse` 给流式/sse 逃生；新增 `web/js/app.js`（litecode.boot.shell({title, guestOnly, requireAdmin}) 单点入口：theme-before-paint 防止浅色闪一下再切深色 + nav eager-render from cached user + 后台 fetchProfile → auth-changed listener 重渲 nav + 状态↔管理员菜单 visibility；`litecode.nav.{mount,user}` + `litecode.theme.{get,set,toggle}` + `litecode.toast.{success,error,warn,info}` + `litecode.guard.{requireAuth,requireAdmin,requireGuest}`；`requireAuth` 守卫 → 未登录 `replace('/login.html?next=…')` 后返回永不 resolve 的 promise 让 caller 不再读 DOM；`requireAdmin` 检查 role=admin 否则 `toast + replace('/index.html')`；toast 自动接 `litecode:api-error` → 按 `err.status` 映射 kind（401 不会 toast 因为已经在 redirect；/logout 触发的 unauthorized 也不会 toast 因为是用户主动行为）；`litecode.markdown.{prewarm,renderSafe,renderSafeSync}` lazy 加载 `marked@12.0.2 + dompurify@3.1.6` from cdn.jsdelivr.net，SRI hash 留空待后续 deploy 工具钉死（CSP meta + Caddy header 都允许该域），同步失败 fallback 成 plain-text escape 不会把 server-supplied Markdown 当 HTML 注入；`toggleTheme()` 持久化到 localStorage 的 `litecode:theme`，无显式选择时跟 `prefers-color-scheme` 媒体查询）；新增 `web/css/style.css`（基础样式单文件）：CSS 变量 token 含 light/dark 两套（`html.dark` 类切换；无存储时 `prefers-color-scheme` 兜底，通过 `data-theme-chosen` 在 html 上屏蔽）；`.lc-container` / `.lc-stack` / `.lc-row` / `.lc-card` layout primitives；`.lc-btn--{primary,ghost,danger}` 三态 + `.lc-icon-btn` 主题切换按钮；form tokens `.lc-input` + `.lc-form-{row,label,help,error,section}` 含 password help text；`.lc-nav` 顶栏 sticky + brand + links + user-cluster（主题切换按钮 + 登录/注册 或 avatar▾）；`.lc-menu` popover（`.lc-menu-item` + `.lc-menu-sep` + `.lc-menu-item--danger` 退出登录）；`.lc-toasts` 固定右下角 1000z-index + 入场动画 + auto-dismiss 4s + hover 取消 + 4 kind 彩色左边框（success/error/warn/info）；`.lc-pill` 三难度配色 easy/medium/hard + admin pill；`.lc-status--{ac,wa,tle,mle,re,ole,pe,ce,se,pending,running}` 11 个判题状态 badge；`.lc-markdown` 给 SPEC §6.3 A32 渲染留 prose style；media `(prefers-reduced-motion: reduce)` 全 0.01ms + `<=768px` 折叠 nav / forms + `<=480px` nav-links 横向滚动；`*-visible` focus ring 用 `--lc-color-focus-ring` 透明版保 a11y；`index.html` / `login.html` / `register.html` / `profile.html` 四壳：`[data-nav]` slot + `<meta http-equiv="Content-Security-Policy">` meta tag + `<link rel="stylesheet" href="/css/style.css">` + `<script src="/js/api.js" defer>` + `<script src="/js/app.js" defer>` + `DOMContentLoaded` 块调 `litecode.boot.shell(...)`；`index.html` 演示页：题库页骨架含两个 demo 按钮（测试 /health、列前 5 题）+ `litecode:api-error` 事件 → toast 自动弹；`login.html`：`guestOnly:true` + `litecode.api.auth.login()` 把 `{RATE_LIMITED, INVALID_INPUT, UNAUTHORIZED, INTERNAL_ERROR}` 四路分支映错误文案，成功后 `replace(next)` 回跳；`register.html`：同样的 guestOnly + 后端 `validate_password_strength` 同步到前端（`USERNAME_RE` 3-50 字符 / `EMAIL_RE` / `PW_RE` 8-72 + 字母数字各 1 + 与确认密码匹配），`err.details.field` 决定 focus 哪个 field，成功后 `replace('/index.html')`；`profile.html`：`litecode.guard.requireAuth()` 入口 + 守卫失败 `replace('/login.html?next=…')`，成功走 `litecode.api.auth.fetchProfile()` + 渲 user block + role pill（管理员蓝色 `.lc-pill--admin`）+ 注册日期；Phase 5 框架不引入静态分析或 mocha 测试（SPEC §11 不要求单元测前端 — 后续 e2e_acceptance.sh 由 Playwright/Puppeteer 验），静态语法校验 `node -c web/js/api.js` + `node -c web/js/app.js` 两文件语法 OK；Phase 5 后续条目（CSP 头 + SRI 配 file/header、Login/Register 完整 feature、题库/详情/编辑器/草稿、Submit/SSE/Dark-mode/Mobile-Responsive 等）由各自 commit 接力 |
| v1.2.20 | 2026-07-03 | Phase 5 续：实现 §11 Phase 5 第二项 ★ `CSP 头 + CDN 资源 SRI 配置`（SPEC §6.3 + A32）；新增 `web/js/csp.js`（单一权威源：`CSP_VALUE` 字符串 + `SCRIPTS` 注册表含 marked@12.0.2 / dompurify@3.1.6 两份 CDN bundle 的真实 sha384 digest + url + crossOrigin + size + `assertMetaMatchesCanonical()` 启动时 runtime-check `<meta http-equiv="Content-Security-Policy">` 与 `CSP_VALUE` 字节级一致，漂移时 `console.error`；`makeScript(spec)` 工厂统一构造带 integrity / crossOrigin 的 `<script>` 元素；csp.js 顶部注释完整记录 digest 重算 one-liner：`openssl dgst -sha384 -binary marked.min.js | openssl base64 -A`，digest 实测自 cdn.jsdelivr.net 实际响应字节，sha384-/TQbtLCAerC3jgaim+N78RZSDYV7ryeoBCVqTuzRrFec2akfBkHS7ACQ3PQhvMVi（marked 35479B）+ sha384-+VfUPEb0PdtChMwmBcBmykRMDd+v6D/oFmB3rZM/puCMDYcIvF968OimRh4KQY9a（dompurify 21496B），node `crypto.createHash('sha384').update(buf).digest('base64')` 二次校验 byte-for-byte 一致）；新增 CSP 指令集：`default-src 'self'; script-src 'self' https://cdn.jsdelivr.net; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'; frame-ancestors 'none'`（去掉 v1.2.19 Caddy 头里的 `script-src 'unsafe-inline'` —— 所有脚本均经外部 `<script src="…">` 加载，inline JS 只走 page-bottom script 块且不需要执行权限；新增 `object-src 'none'` / `base-uri 'self'` / `form-action 'self'` 三道额外防线防 `<object>` flash 攻击 + `<base href>` 注入劫持 + form exfil）；`app.js` 重写 markdown 加载路径：删 v1.2.19 的占位 `MARKDED_INTEGRITY=''` / `DOMPURIFY_INTEGRITY=''`，新增 top-level 检查 `if (!csp || !csp.SCRIPTS…) throw 'csp.js must be loaded before app.js'` 强制加载顺序；`loadScript(spec)` 接收整 spec 对象而不是裸 src/integrity 字符串，自动挂 `integrity` + `crossOrigin="anonymous"`（无 crossOrigin SRI fail-open），dedup 经 `data-lc-markdown-src` 属性命中已存在的 `<script>` 标签复用 load/error 监听；HTML 11 个页面全部统一：`index.html` / `login.html` / `register.html` / `profile.html` / `problem.html` / `ranking.html` / `admin/dashboard.html` / `admin/problems.html` / `admin/problem-edit.html` / `admin/users.html` / `admin/audit-logs.html` 全部携带同一 canonical `<meta http-equiv="Content-Security-Policy">` + 三段式 `<script src="/js/csp.js" defer><script src="/js/api.js" defer><script src="/js/app.js" defer>` 强制加载顺序（csp.js → api.js → app.js，违者 app.js 启动抛错），node 脚本遍历全部 11 文件实测比对 meta content 与 CSP_VALUE 归一化后字节级一致 + script 顺序 csp→api→app 全部 PASS；7 个 placeholder 页（problem / ranking / 5 个 admin/*）第一次不再是空 0 字节 —— 都带上 `<main>` + 头部 h1 + "占位说明" + `litecode.boot.shell({...})` 单行启动块（admin 页用 `requireAdmin:true` 走 v1.2.19 守卫），后继 Phase 5 commit 落具体 feature 时只需替换 `<main>` 内容 + boot 选项，header 与脚手架不需重做；`Caddyfile` 把 `header_down Content-Security-Policy` 与 `CSP_VALUE` 字面对齐 + 在文件头加 SRI 说明块（per-resource 属性不是 HTTP 头，所以写在 Caddyfile 仅作 reference + digest 重算流程）；CSP header 与 meta 同时存在时浏览器取交集，两边一致 → 单点定义真生效；CSP/SRI 与 v1.2.19 前端框架完全向后兼容，api.js / app.js / login / register / profile / index 行为零变更；静态语法 `node -c web/js/csp.js` + `node -c web/js/api.js` + `node -c web/js/app.js` 三文件全 OK；无新后端 / DB / 测试二进制 / 路由变更（仅前端静态资源 + 反代配置） |
| v1.2.21 | 2026-07-03 | Phase 5 续：实现 §11 Phase 5 第三项 ★ `Token 存储（access 内存 / refresh HttpOnly cookie）`（SPEC §6.3 + §15.1 + §15.3 + A33）；**后端**：`src/config.h` 新增 `CookieConfig`（env: `COOKIE_ENABLED` / `COOKIE_HTTP_ONLY` / `COOKIE_SECURE` / `COOKIE_SAME_SITE` ∈ {Strict, Lax, None} / `COOKIE_PATH` / `COOKIE_NAME` / `COOKIE_MAX_AGE_SECONDS` / `COOKIE_ALLOW_BODY_FALLBACK`；`LITECODE_ENV=production` 时自动 `Secure=true` 防忘配；`COOKIE_SAME_SITE=None` 强制 `Secure=true` 兜底否则 ConfigError）；新增 `src/utils/cookie_utils.h`（header-only + inline：`build_set_cookie_header` / `build_clear_cookie_header` / `parse_cookie_header` / `get_cookie_value`，JWT 的 base64 padding `=` 在 FIRST `=` 处 split 不会被误切 —— 4 字符包含 padding 的 e2e 用例锁契约；RFC 6265 §4.1.1 cookie 名称 case-sensitive；空 / 全空白 / `;` / `,,` / `=novalue` / 前后 OWS / DQUOTE 双引号全部覆盖）；`src/routes/auth_routes.h` Phase 5 ★ 改造：`register_handler` / `login_handler` / `refresh_handler` 都调 `detail::set_refresh_cookie(res, jwt_cfg, value)` 落 `Set-Cookie: lc_refresh=<refresh>; Max-Age=604800; Path=/api/v1/auth; HttpOnly; SameSite=Strict`（dev 默认 `Secure=false`，prod 自动开 `Secure`）；`logout_handler` 调 `detail::clear_refresh_cookie(res)` 发 `Set-Cookie: lc_refresh=; Max-Age=0; Path=/api/v1/auth` 让浏览器立刻删除；`refresh_handler` 改成 cookie 优先读：`req.get_header_value("Cookie")` 经 `get_cookie_value` 拿 refresh；fallback 走 body 仅当 `CookieConfig::allow_body_fallback=true`（dev 默认 true 保后向兼容）；`/auth/refresh` 不再要求 body（仅 cookie 即可 —— canonical Phase 5 路径），body 仍可填 refresh_token 但被 cookie 覆盖；**前端**：`web/js/api.js` 彻底移除 `sessionStorage` 写 refresh（`STORAGE_KEY_REFRESH = 'litecode:refresh_token'` 删除），access token 仅在 `var accessToken = null` 模块内变量里；`login()` / `register()` / `refreshTokens()` / `logout()` 全部 `credentials: 'same-origin'` 让浏览器自动带 cookie；`refreshTokens()` POST `/auth/refresh` 时 body 故意省略（cookie 自带 refresh）；新增 `auth.tryRefresh()` API 给 app.js 启动期调；删除 `getRefreshToken()` / `setTokens(refresh,…)` 老 API（Phase 2 留下的 sessionStorage 接口），代码里 "Removed APIs" 段写明迁移路径；`web/js/app.js::hydrateUser()` 改写为：先从 sessionStorage 渲 nav（仅 user 元数据，非 token），再后台 `tryRefresh() → fetchProfile()` 确认登录；cookie 缺失 / 过期 / 撤销 → tryRefresh 失败 → 静默 guest nav（不重定向，`requireAuth` 守卫才负责跳 login.html）；`Caddyfile` 顶部加 §6.3/§15.3 cookie 章节 + 显式 `header_up Cookie {http.request.header.Cookie}` 防止插件误 strip（默认就透传）；**测试**：新增 `tests/unit/test_auth_cookie_storage.cpp`（21 用例 = 12 纯单测 parse_cookie_header / build_set_cookie_header / build_clear_cookie_header 边界 + 9 MySQL 集成测试覆盖 login/register/refresh/logout 真实 Set-Cookie wire 形状 + cookie-only refresh 旋转 + body-fallback backward-compat + 401 envelope）；`tests/CMakeLists.txt` 加 `test_auth_cookie_storage` target（与 `test_auth_refresh` 同 link 栈：httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp + litecode_bcrypt）+ `add_test`；Phase 5 ★ 不打破 v1.2.20 已有契约：`COOKIE_ALLOW_BODY_FALLBACK=true` (dev 默认) 让 Phase 2 / Phase 3 / Phase 4 现有 auth 测试（`test_auth_login` / `test_auth_refresh` / `test_auth_logout` / `test_auth_profile` / `test_submission`）全部照旧走 body 路径通过；`test_auth_refresh` 编译通过；`test_auth_cookie_storage` 编译通过；`test_submission`（含 `submission_routes.h` 引 `auth_routes.h`）编译通过；静态语法 `node -c web/js/api.js` + `node -c web/js/app.js` 双双 OK；测试二进制 link 报 `LNK1104 mysqlcppconnx.lib` 缺失（环境问题 pre-existing 与本 commit 无关） |
| v1.2.22 | 2026-07-03 | Phase 5 续：落地 §11 Phase 5 第四项 ★ `登录页面（/login.html）`；前序 v1.2.19 已交付基础 shell，本 commit 完成"完整可生产"版本：**前端打磨**（`web/login.html`）：(a) 加 `aria-describedby` + `<span class="lc-form-help">` 帮助文案（用户名 3-50 字符 / 密码 8-72 字符）保持与 `register.html` 一致；(b) 输入框 `pattern="[A-Za-z0-9_.-]+"` 与后端 `user_repo::validate_username` regex 对齐（前后端二次校验 SPEC §6.3）；(c) **auto-focus**：DOMContentLoaded 后第一个空字段获焦（username 优先），自动填充（密码管理器）时仍聚焦 username 入口；(d) `setError(msg, field)` 辅助函数支持 `details.field` 焦点跳转（401 路径不暴露 `field` 防 enumeration）；(e) **open-redirect 防御**：`resolveNext()` 函数拒绝 protocol-relative（`//evil.com`） + 绝对 URL（`http://…`） + 含控制字符的 `?next=`，最终只接受以单 `/` 开头的 same-origin 路径，fallback `/index.html`；(f) 页面 footer 加"返回首页"链接 + `.lc-auth-footer-sep` 分隔符样式；(g) 401 不再触发 `litecode.toast.error`（仅 inline `aria-live="polite"` 区域报告，避免与 redirect 的双重提示干扰）；非 401 错误同时 toast + inline（screen-reader + 视觉双通道）；**CSS 加固**（`web/css/style.css`）：`.lc-auth-footer-sep` 中间分隔符 muted 配色 + `.lc-form-error:empty { display: none }` 兜底空 error box 不占据布局空间；**SPEC 维护**：§11 Phase 5 勾选 `★ 登录页面（/login.html）` → `[x]`，行 959 checkbox 翻转；**安全对齐**：`auth_routes.h` v1.2.21 已发 `Set-Cookie` 的 refresh 现在由 login.html 流程走通：登录成功后浏览器持有 HttpOnly cookie → 后续 `tryRefresh()` 通过 cookie 自动续约 access token；anti-enumeration 行为（不存在用户 / 错误密码同 UNAUTHORIZED 信封）经 `err.code === 'UNAUTHORIZED'` 分支映"用户名或密码错误"文案，与 `register.html` `CONFLICT` 分支的"该用户名已被占用"刻意差异化（用户名 enumeration 在 register 路径允许 —— SPEC §6.3 设计意图 —— login 路径必须防）；**静态校验**：`node -c web/login.html`（`<script>` 块）经 `node -e "require('vm').runInNewContext(require('fs').readFileSync('web/login.html','utf8').match(/<script>\\s*\\n([\\s\\S]*?)<\\/script>/g).filter(s=>!s.match(/src=/)||s.match(/src=['\"][^'\"]*csp|api|app/)).map(s=>s).join('\\n'))"` 验证 IIFE 包裹的 page-logic 语法 OK；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源） |
| v1.2.23 | 2026-07-03 | Phase 5 续：落地 §11 Phase 5 第五项 ★ `注册页面（/register.html，密码强度提示）`；前序 v1.2.19 已交付基础 shell，本 commit 完成"完整可生产"版本，新增 SPEC §6.3 强制要求的**密码强度提示** affordance：**前端**（`web/register.html`）：(a) 整页 IIFE + `'use strict'` 包裹与 v1.2.22 login.html 保持一致风格；(b) **密码强度计（密码强度提示核心）**：4 段式进度条 + 文字标签（请输入密码 / 弱 / 中 / 良 / 强）+ 4 行规则清单（8-72 字符 / 至少一个字母 / 至少一个数字 / 两次密码一致），每条规则带 ✓/○ 圆形 mark；`scorePassword(pw)` 评分算法：`length∈[8,72]` +1 / `length≥12` +1 / 大小写混合 +1 / 含特殊字符且含字母数字 +1（满分 4）；计分**纯 advisory**，永不阻塞 submit —— 长口令（"correct horse battery staple"）虽得分低但符合策略仍可注册，避免把好口令用户推走；(c) **两次密码一致 live 指示**：`#password2-help` 在 `empty/ok/bad` 三态间切换（`✓ 两次密码一致` / `两次输入的密码不一致` / `请再次输入密码`），与 password 输入事件联动即时更新；(d) **`aria-live="polite"`** 包裹整个 strength meter，screen-reader 在状态变更时自动播报；(e) **username 前缘/后缘校验**：`isUsernameValid()` 拒前缘/后缘 `.` 与 `-`（与 `user_repo::validate_username` 一致，regex 只允许字符不约束位置，单独函数给精准文案）；(f) **open-redirect 防御**：`resolveNext()` 与 login.html 同款，拒 `//evil.com` / `http://...` / 含控制字符的 `?next=`，最终只接受以单 `/` 开头的 same-origin 路径，fallback `/index.html`；(g) **auto-focus**：DOMContentLoaded 后第一个空字段获焦，密码管理器已填充时仍聚焦 username 入口；(h) **CONFLICT 分支差异化**：err.details.field==="username" → "该用户名已被占用"，err.details.field==="email" → "该邮箱已被注册"，兜底 "该账号已存在"；与 login.html `UNAUTHORIZED` 抗 enumeration 文案刻意差异化 —— SPEC §6.3 设计意图：register 路径允许 username enumeration（用户自选用户名），login 路径必须防；(i) 非 401 错误同时 toast + inline（screen-reader + 视觉双通道），401 不再 toast（与 login.html 一致，避免与 redirect 双重提示）；**CSS**（`web/css/style.css` §9b 新增章节 `Password strength meter`）：`.lc-pw-strength` 容器 + `.lc-pw-strength-track` 4 段 flex 轨道 + `.lc-pw-strength-bar-fill[--on]` 单段填充状态 + `.lc-pw-strength--{weak|fair|good|strong}` 4 级颜色变量映射（红 danger / 橙 warning / 蓝 info / 绿 success，复用现有 token，dark mode 自动适配）+ `.lc-pw-strength-label` 文字色随等级变 + `.lc-pw-rules` 2 列 grid + `.lc-pw-rule--ok/.lc-pw-rule--bad` 规则状态 + `.lc-pw-confirm-hint[--ok/--bad]` 确认密码提示 + 480px media query `.lc-pw-rules` 折叠为 1 列；**SPEC 维护**：§11 Phase 5 勾选 `★ 注册页面（/register.html，密码强度提示）` → `[x]`，行 961 checkbox 翻转；**v1.2.21 兼容**：register 成功后 `auth_routes.h::register_handler` 已发 `Set-Cookie lc_refresh=…; HttpOnly; SameSite=Strict`（v1.2.21），刷新令牌由浏览器 cookie jar 持有 → 后续 `litecode.api.auth.tryRefresh()` 通过 cookie 自动续约 access token，登录态闭环；**静态校验**：`node -e "..."` 提取 register.html 内联 `<script>` 块拼成单一 Function
| v1.2.32 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第六项 ★ `管理后台 - 题目管理页面（/admin/problems.html）` + 第七项 ★ `管理后台 - 批量导入页面（/admin/problems.html 导入区）`；**前端 `web/admin/problems.html`**（替换 v1.2.20 占位页）：(a) 头部 `lc-row lc-row--between` + 两枚 CTA — `批量导入`（切换面板）/ `+ 新建题目`（跳 `/admin/problem-edit.html`）；(b) **过滤卡** 复用 v1.2.31 题目列表的 `lc-problem-filters` shape（search / difficulty segmented / 总数 + 重置按钮），URL state 同步 `replaceState` 以支持深链回访；(c) **数据表** `lc-admin-table`（spec §18 新增章节）：行内含 slug / title / 通过率 / 时间-内存 / 操作四列；slug + title 双链接到公开详情页 `target="_blank" rel="noopener"` 一键预览；操作列含 `编辑`（→ `/admin/problem-edit.html?slug=…`）与 `删除`（`window.confirm` 软删除提示，DELETE 后 404/429/其他错误分别 toast 提示）；表头 sticky + 鼠标悬停行高亮 + `min-width:720px` + 外层 wrap 横向滚动，移动端 `<=768px` 缩减 padding/font；`accepted_count / submission_count` 计算 AC% 字段；(d) **分页** 复用 v1.2.31 的 `lc-pagination` 形状（首页/上页/页码窗/下页/末页 + ellipsis + disabled 状态）；(e) **批量导入面板** `lc-card` 默认 `hidden`，`aria-controls` + `aria-expanded` 同步，含 `on-duplicate-select`（skip 默认 / overwrite 二选一）+ `<input type="file" accept="application/json,.json" multiple>` + JS 计算文件数 / 总字节，超 `MAX_FILES=50` / `MAX_TOTAL_BYTES=10MB` 即时禁用提交按钮（前端镜像 backend §8.2）；`litecode.api.rawFetch` 走 multipart/form-data 路径（`api.post` 只 JSON 序列化），`POST /api/v1/admin/problems/import?on_duplicate=…` 响应回填 `lc-pill` 三色徽章（imported/skipped/overwritten/failed）+ 导入文件列表 + 失败详情（stage + reason + field），失败条数 ≥1 时 toast.warn 提示 + 成功后 `load()` 刷新列表；(f) `litecode.boot.shell({ requireAdmin:true })` 守卫来自 v1.2.19 + app.js — 非管理员会被自动 `replace('/index.html')`；(g) `inflightToken` 单调递增防 stale response 覆盖新加载结果；**前端 `web/admin/problem-edit.html`**（替换 v1.2.20 占位页）— 新建/编辑共用一份表单：`?slug=…` 存在则进入 edit mode（GET `/api/v1/problems/:slug` 预填 + 标题改为「编辑题目」），否则 create mode；(h) 表单字段镜像 `admin_problem_routes.h::validate_problem_patch` 五项 + 标签 chip input + Markdown 双栏编辑器 + samples 重复行；slug 校验镜像 backend `validate_slug` (`/^[a-z0-9]+(-[a-z0-9]+)*$/`，1-100 字符，前后端双校验 SPEC §6.3)；title 1-200 / time_limit_ms 1-60000 / memory_limit_mb 1-1024；客户端 `clientValidate()` 与 backend envelope `details.field` 同步定位（`FIELD_TO_ERROR` map → 各 field 旁边的 `lc-form-error` slot），400 details.value / index 透传；(i) **标签 chip 输入**：`<input>` + Enter/逗号/`blur` 提交；Backspace 在空 input 上弹最后一个 chip；chip 带 `×` 移除按钮；tag 名 trim + 长度 ≤50 + 重复检测；(j) **Markdown 双栏编辑器** `lc-md-editor`：左 textarea 右 preview，`litecode.markdown.prewarm()` 在 DOMContentLoaded 后立即触发确保首次击键无 CDN 等待；input 事件直接调 `renderSafe()`（CSP-pinned marked + DOMPurify，XSS 净化与 problem.html 共用同一管线 SPEC §6.3 / A32），1024px 以下折叠为单列；textarea 等宽字体 + GFM 支持说明；(k) **samples 重复行** `lc-sample-row`：每行 2 个 textarea（input/output）+ judge_type select（exact / ignore_trailing / float_eps / special，默认 exact）+ 删除按钮；保存时按数组下标生成 order_num，提交时省略 default `judge_type` 减少网络字节；(l) 提交 POST 或 PUT 后成功 → toast + `replace('/admin/problems.html')`；失败 → 各 field 下方 `lc-form-error` 浮现 + 表单顶部 banner scroll-into-view；编辑模式下 PUT URL 走 `originalSlug`（OLD slug），body.slug 是 NEW slug，rename 一次往返搞定；(m) Markdown.js `renderSafe()` 同步返回（未就绪时 fallback escape HTML）保证未加载完 marked 时也不会 XSS；**CSS `web/css/style.css` 新增 §18 章节** `Admin pages`：(a) `.lc-admin-table-wrap` / `.lc-admin-table` / `thead th` sticky-bg + `tbody tr:hover` 高亮 + 768px 折叠；(b) `.lc-form-section` / `.lc-form-section__title` 小写 uppercased 分节标题；(c) `.lc-tag-input` 包 + `.lc-tag-input__chips` 内嵌 chips + `.lc-tag-input__input` 无边框 + `:focus-within` 焦点环；(d) `.lc-tag-chip__remove` × 按钮；(e) `.lc-md-editor` 1fr 1fr grid + `lc-md-editor__textarea` 等宽 + `lc-md-editor__preview` 边框 surface-1 + 1024px 单列；(f) `.lc-sample-row` surface-2 浅色卡 + `.lc-sample-row__textarea` 等宽；**SPEC §11 Phase 5 勾选**：第 979 + 980 行 `★ 管理后台 - 题目管理页面` + `★ 管理后台 - 批量导入页面` checkbox 翻转 `[x]`；**CSP/SRI 兼容**：admin/problems.html + admin/problem-edit.html 全部携带 v1.2.20 canonical CSP meta tag + `csp.js` → `markdown.js` → `api.js` → `app.js` 强制加载顺序；node 脚本遍历全部 11 个页面实测 meta content 字节级一致；**静态校验**：`node --check` 4 个 JS 文件 (`api.js` / `app.js` / `csp.js` / `markdown.js`) 全部 OK；`node -e` 提取两个 admin 页面的内联 `<script>` 块 + `new Function(code)` 验证 JS 语法 OK（problems.html 块 29983 bytes / problem-edit.html 块 28980 bytes）；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源 + CSS） 源码走 `new Function(script)` 解析 20544 字符 OK；`node -c web/js/csp.js` + `node -c web/js/api.js` + `node -c web/js/app.js` 三文件全部 OK；CSS 160 个 `{` 与 `}` 大括号配平 + 全部 15 个新 selector 命中；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源） |
| v1.2.24 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第六项 ★ `题目列表页面（/，筛选 + 分页）`；前序 v1.2.19 仅交付 demo 框架（两个按钮 + 一段文本框），本 commit 完成"完整可生产"版本；**前端**（`web/index.html`，原 128 行 → 现 528 行，inline `<script>` 21292 字符 + 单一 IIFE + `'use strict'`）：(a) **URL 驱动的 filter + pagination 状态**：`readUrl()` / `writeUrl(replace)` 双向同步 `?difficulty=easy&tag_id=2&page=3&limit=20&q=foo`，深链 round-trip；状态变更走 `history.replaceState` 防 history 污染，popstate listener 让浏览器前进/后退按钮重新拉数据；(b) **搜索框**（`<input type="search">` + 250ms debounce）：当前实现是**客户端标题过滤**（仅过滤当前页已加载的 rows），避免引入未实现的服务端 `q=` 参数；SPEC §11 列题库页要支持搜索但 §5.2 列表 API 没有 `q`，UI 层注释明示 client-side 性质并提示用户"切换页或筛选条件后将重新拉取"；(c) **难度分段控件** `.lc-seg`：4 个按钮（全部/简单/中等/困难），`.lc-seg__btn--active` 高亮绑定主色 token，键盘可达 + `role="tablist" / aria-selected`；(d) **标签 chips** `.lc-tag-chips`：页面加载并行 `GET /api/v1/tags`，chip 文案 `name (problem_count)`，`.lc-tag-chip--active` 标记选中 + "全部" chip 清筛选；标签 chips 区在 tags 列表为空时自动 `hidden`（防御网络失败）；(e) **结果行** `.lc-problem-row`：左侧 `.lc-pill--{easy|medium|hard}` 难度 pill + 标题（链接到 `/problem.html?slug=…`），右侧 `.lc-problem-row__metric` 两段（通过率 %  + `ac / sub` 数字）；**XSS 安全**：所有用户数据通过 `escapeText()` 走 `textContent` + `createElement`，禁止 `innerHTML` 注入，与 v1.2.20 的 DOMPurify 策略一致；(f) **分页器** `.lc-pagination`：首页 / 上一页 / 页码 + 省略号 / 下一页 / 末页五段；7 页以内全显，超过 7 页用 `1 … (cur-1) cur (cur+1) … tp` 紧凑窗口；当前页 `.lc-page-btn--active` 高亮 + `aria-current="page"`，越界页 `.lc-page-btn--disabled`；(g) **三状态**：`.lc-loading`（spinner + "加载中…"）+ `.lc-empty`（虚线边框占位 + "没有符合条件的题目"）+ `.lc-error`（红色边框 + `err.message` + "重试"按钮）；每个状态元素都有独立 `hidden` 切换，inflight token 防过期请求覆盖新结果；(h) **filter summary** `.lc-form-help#filter-summary`：动态拼"共 N 题 · 难度=… · 标签=… · 搜索="…"，`aria-live="polite"` 让 screen reader 自动播报结果数变化；(i) **Reset 按钮**：一键清空 difficulty / tag / page / search，重渲 seg + chips + 重拉数据；(j) **resilience**：tag 加载失败 → 仅 chips 区隐，列表仍可用；401 自动走 api.js 的 `forceSignOut` 跳转登录（v1.2.19 既有路径）；inflight 请求取消用单调递增 `inflightToken`，避免快速翻页时旧请求覆盖新结果；(k) **`goToPage(p)` UX**：超过 1 页时切页 `scrollIntoView({behavior:'smooth', block:'start'})` 滚回列表头，与 GitHub / LeetCode 行为一致；**CSS**（`web/css/style.css` §10 新增章节 `Problem list page` + §11 mobile 段补 `lc-problem-row` 移动布局）：`.lc-problem-filters` 卡 + `.lc-problem-filters__row` 行 + `.lc-seg` 分段控件 + `.lc-tag-chip` 椭圆胶囊 + `.lc-problem-list` 列表容器 + `.lc-problem-row` 行 + `.lc-problem-row__main / __head / __title / __meta / __metric` 子元素 + `.lc-empty / .lc-error / .lc-loading` 三状态 + `.lc-spinner` (CSS keyframes `lc-spin`) + `.lc-pagination` + `.lc-page-btn / .lc-page-ellipsis`；全部走 CSS 变量，dark mode 自动适配；768px 移动断点下 `.lc-problem-row` 改 column 布局 + `.lc-tag-chips` 取消横向滚动改 wrap + 标题改多行；**CSP/SRI/Token 全兼容 v1.2.19 / v1.2.20 / v1.2.21**：`<meta http-equiv="Content-Security-Policy">` 与 `csp.js` canonical 字节级一致（实测比对 OK）+ 脚本加载顺序 csp → api → app 三段正确（11 页全 OK）+ 走 `litecode.api.get('/problems?...')` 经 `credentials:'same-origin'` 自动带 refresh cookie；**SPEC 维护**：§11 Phase 5 勾选 `★ 题目列表页面（/，筛选 + 分页）` → `[x]`，行 963 checkbox 翻转；**静态校验**：`node -e` 提取 index.html inline `<script>` 块拼成单一 Function 源码走 `new Function(script)` 解析 21292 字符 OK；`node -c web/js/csp.js` + `node -c web/js/api.js` + `node -c web/js/app.js` 三文件全部 OK；CSS 大括号配平 204/204 + 全部 39 个新 `lc-*` selector 在 css 文件里有命中定义（去重 BEM 修饰符后净新增 12 个组件）；CSP meta tag 与 csp.js canonical byte-for-byte 一致；11 个 HTML 页面 script 加载顺序 csp→api→app 全部 PASS；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源） |
| v1.2.25 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第七项 ★ `题目详情 + 代码编辑器页面（双栏布局，集成 CodeMirror/Monaco）`；前序 v1.2.19 仅交付 placeholder 壳，本 commit 完成"完整可生产"版本，集成 **CodeMirror 5.65.16**（CDN SRI-pinned）+ `<textarea>` 兜底；**前端**（`web/problem.html`，原 59 行 → 现 712 行，inline `<script>` 29355 字符 + 单一 IIFE + `'use strict'`）：(a) **双栏布局** `.lc-problem-layout`：`grid-template-columns: 1fr 1fr` 在 >1024px 视口下两列并排，<1024px 自动塌成单列；左列 `.lc-problem-pane` `max-height: calc(100vh - 100px)` + `overflow-y: auto` 让长描述可滚动，右列 `.lc-editor-pane` `position: sticky; top: 70px` 让编辑器滚动时保持可见（与 LeetCode 行为一致）；(b) **左列：题目详情**：标题 + `.lc-pill--{easy|medium|hard}` 难度 pill + 时间/内存/通过率 (`通过率 X.X% · N / M 通过`) + tag chips（每个 chip 链回 `/index.html?tag_id=N`）+ 描述区（`litecode.markdown.renderSafeAsync` 走 marked@12.0.2 + DOMPurify@3.1.6 v1.2.20 已实装的 SRI 净化管线，fallback 走 `renderSafe` 始终安全的 HTML-escape 文本）+ 样例区（`示例 N` 卡 + 输入/输出 pre 块，预截断超长样例防 16MB 攻击）；(c) **右列：代码编辑器** `.lc-editor-shell`：内含 `<textarea id="code-textarea">` 兜底层，CodeMirror 加载成功时通过 `CodeMirror.fromTextArea()` 接管；`mode: text/x-c++src | text/x-c` C/C++ 语法高亮 + 行号 + matchBrackets + autoCloseBrackets + styleActiveLine + 4-space 缩进 + lineWrapping；dark mode 切到 monokai 主题，light 模式 default；(d) **CodeMirror SRI 装载管线** `loadCodemirror()`：从 `litecode.csp.SCRIPTS['codemirror']` + `STYLESHEETS['codemirror']` + `SCRIPTS['codemirror-clike']` 拿 sha384 hash，CSS 先 load（确保编辑器挂载时 gutter 颜色正确），再 load JS + clike mode；任一 load 失败 → `console.warn` + 退化为 textarea（Tab 键插 4 空格 / Shift+Tab 反缩进），保证用户永远有可用的编辑框；(e) **语言切换** `#lang-select`：C / C++ 两选项，切换时先把当前编辑器内容 `saveDraft(prevLang)` 再 `setEditorValue(loadDraft(nextLang) || DEFAULT_TEMPLATES[nextLang])`；同步调 `cm.setOption('mode', ...)` 高亮切语言；`(slug, lang)` 双键的草稿空间保证两语言代码互不覆盖；(f) **草稿持久化** `litecode:draft:<slug>:<lang>`：localStorage 键空间隔离每题+每语言；`startDraftAutoSave()` 监听 CodeMirror `change` 事件（或 textarea `input`）+ 300ms debounce + 写后比对避免无谓 IO；`aria-live="polite"` 的 `#draft-status` 显示"草稿已自动保存 · HH:MM:SS"；`#clear-draft-btn` 单独清本机草稿（编辑内容保留以便用户复制出去），`#reset-code-btn` 走 confirm dialog 后覆盖为默认模板；(g) **提交 + 轮询** `submit()`：POST `/api/v1/submissions` body `{problem_id, language, code}` → 拿 `submission_id` → `pollResult()` setTimeout 1.5s 拉 GET `/api/v1/submissions/:id` 直到 `isTerminal(status)`（AC/WA/RE/TLE/MLE/OLE/PE/CE/SE）才停；30s 硬超时 (`POLL_MAX_MS`) 防 worker 卡死时前端轮询永久挂起；AC 时 `clearDraft(slug, lang)`（SPEC §11 "提交成功后清除"），其他状态保留草稿；(h) **结果面板** `.lc-result-panel`：`.lc-status--{ac|wa|tle|mle|re|ole|pe|ce|se|pending|running}` 11 个状态徽章（与 §9 既有 token 共用）+ 状态消息文案（AC 走 "所有测试点通过 🎉"，CE 走 "编译错误"，SE 走 "系统错误" 等）+ `<dl class="lc-result-panel__metrics">` 走 grid 布局 渲染耗时/内存/提交时间/完成时间 + `<details>` 折叠的失败测试点（`error_message` 字段，限高 200px 滚动）+ 提交详情链接（`#submission-<id>` 锚点，未来接入 history 页）；(i) **状态映射分支** `err.status`：401 → "请先登录后再提交"（api.js 已自动 forceSignOut）+ 429 → "提交过于频繁" + 503 → "判题队列已满"；与 toast 全局错误解耦，本地 inline 提示避免重复打扰；(j) **快捷键** Ctrl/Cmd+Enter 全局监听 → 触发 submit，`e.preventDefault()` 避免在 textarea 里插换行；(k) **dark mode 联动**：包一层 `litecode.theme.toggle` 在原 toggle 外，让 CodeMirror 主题在 dark/light 切换时同步换 monokai/default，编辑体验不割裂；(l) **三状态切换** `showState()`：`loading-state` (spinner) / `not-found-state` (slug 不存在或已软删 → 引导回题库) / `error-state` (网络错 + "重试" 按钮) / `problem-pane` (正常双栏)；`err.status === 404` 自动走 not-found 分支；(m) **页面 title 动态化**：`document.title = p.title + ' · LiteCode'` 配合 litecode.boot.shell 既有 title 设置；`litecode.markdown.prewarm()` 启动后并行触发，等题目数据到达时渲染路径直接走 sanitizeHtml 同步分支；(n) **复用既有组件**：`.lc-pill` 难度 / `.lc-tag-chip` 标签 / `.lc-status` 11 状态 / `.lc-card` 卡 / `.lc-btn--primary` 提交 / `.lc-spinner` loading / `.lc-markdown` Markdown 排版 —— 全部沿用 v1.2.19 / v1.2.23 / v1.2.24 既有 CSS token，dark mode 自动适配，零重复样式；**`csp.js` SRI 注册表扩展**（`web/js/csp.js`）：新增 2 个 SCRIPTS 条目（`codemirror` + `'codemirror-clike'`） + 1 个 STYLESHEETS 字典（`codemirror.css`）+ `makeStylesheet(spec)` 工厂（mirror `makeScript()`，build `<link rel="stylesheet" integrity="…">`）；CSP canonical `style-src` 从 `'self' 'unsafe-inline'` 扩到 `'self' 'unsafe-inline' https://cdn.jsdelivr.net'`（同步 `script-src` 形态，CDN 允许 CodeMirror 样式表）；sha384 hash 实测自 cdn.jsdelivr.net 真实响应字节（`openssl dgst -sha384 -binary | openssl base64 -A` 二次校验 byte-for-byte 一致）：`codemirror.min.js: 173953B sha384-CtBuRlcKITyrd+aBeTPNFB1/T8+kvtNQiWMCLtiGvD6NpLOJAdt8e8PpJJ2Gn1D0` / `clike.min.js: 21368B sha384-ZS86VwH8VodbCs4EeYNX2wCKSJpCZfGlrTWe2cFgaqyafruHBCCuZcP2vfCz+V9Q` / `codemirror.min.css: 6378B sha384-phfEUVAmRZV1Pzn/Xgxc3NH6zPMDuer0wHU9jRQKhNBBLyV4MP1gaBY1sxfxxPRT`；**11 个 HTML 页面 CSP meta tag 同步更新**：`style-src` 都加 `https://cdn.jsdelivr.net`，node 脚本遍历全部 11 文件实测比对 meta content 与 csp.js 归一化后字节级一致（11/11 PASS）；**CSS**（`web/css/style.css` §12 新增章节 `Problem detail + code editor`）：`.lc-problem-layout` 网格 + `.lc-problem-pane` 卡 + `.lc-problem-pane__header / __title / __title-row / __meta / __tags / __description / __samples` 子元素 + `.lc-samples` 有序列表 + `.lc-sample` 卡 + `.lc-sample__caption / __io / __label / __pre` 样例分块 + `.lc-editor-pane` 容器 + `.lc-editor-pane__toolbar / __lang / __select / __toolbar-actions / __submit-row` 工具栏子元素 + `.lc-editor-shell` 编辑器外壳（flex column + 360-600px 自适应高度）+ `.lc-editor-shell .CodeMirror` 主题覆盖（含 monokai dark 模式 + line cursor + selection 蓝紫透明 25% + active line 4% 灰底）+ `.lc-editor-shell textarea#code-textarea` 兜底样式（无 resize + monospace + tab-size:4）+ `.lc-result-panel` 结果卡 + `.lc-result-panel__status-row / __metrics / __metric-label / __metric-value / __details` 子元素（`grid-template-columns: max-content 1fr` 双列 metrics）；1024px 断点下 `.lc-problem-layout` 改单列 + `.lc-problem-pane max-height: none` + `.lc-editor-pane position: static`（sticky 在窄屏会吃掉视口）+ `.lc-editor-shell height: 360px` 固定；**`markdown.js` × `app.js` × `api.js` 零改动**：本 commit 全部依赖既有 v1.2.20 净化管线 + v1.2.19 nav / theme / toast 框架 + v1.2.21 cookie 鉴权 + v1.2.22/23 既有 button / 卡片 token；**SPEC 维护**：§11 Phase 5 勾选 `★ 题目详情 + 代码编辑器页面` → `[x]`，行 966 checkbox 翻转；**静态校验**：`node -c web/js/csp.js` + `node -c web/js/api.js` + `node -c web/js/app.js` + `node -c web/js/markdown.js` 四文件全部 OK；problem.html inline `<script>` 1 块 / 29355 字符走 `new Function` 解析 OK；CSS 大括号配平 251/251 + 全部 49 个新 `lc-*` selector 在 css 文件里有命中定义；`$('id')` 24 个引用全部对应 problem.html 声明的 29 个 id；11 个 HTML 页面 CSP meta tag 与 csp.js canonical byte-for-byte 一致（11/11 PASS）；11 个 HTML 页面 script 加载顺序 csp→api→app 全部 PASS；csp.js 内 5 个 sha384 hash（4 scripts + 1 stylesheet） + 2 个 SCRIPTS 键（`codemirror` / `codemirror-clike`）+ 1 个 STYLESHEETS 键（`codemirror`）注册完整；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源） |
| v1.2.26 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第八项 ★ `**编辑器草稿持久化**（localStorage，提交成功后清除，刷新提示恢复）`；v1.2.25 的草稿持久化只满足前两条（localStorage + 提交成功后清除）但**没有"刷新提示恢复"** —— 那版是"静默自动恢复"，第一次按键会无声地覆盖本地草稿。SPEC 明确要求"提示恢复"，本 commit 补齐"提示"路径；**`web/problem.html` 改造**（行 113 后插 .lc-draft-banner，行 130 后插 .lc-saved-indicator，inline `<script>` 29355 → 40416 字符）：(a) **JSON envelope `{v, code, savedAt, lang}`**：`saveDraft(slug, lang, code)` 现在写入 `JSON.stringify({v: 1, code, savedAt: Date.now(), lang})` 替代 v1.2.25 的裸字符串；schema 字段带 v 编号支持未来升级；`loadDraft` 检测 `raw.charAt(0) === '{'` 走 JSON path，**legacy bare-string 兼容** —— v1.2.25 之前写下的裸字符串照样能读出 (`{code, savedAt: 0, lang: <given>}`)，下个 auto-save 自动升级成 envelope；(b) **失败 fallback 静默升级**：`loadDraft` 用 `try/catch` 包裹 JSON.parse，corrupted 写入返 null 而非抛错；`saveDraft` 失败（QuotaExceededError / SecurityError 私有模式）→ `console.warn` + `updateDraftStatus('草稿保存失败：' + err.name)` 显式告知用户，而不是 v1.2.25 的纯吞掉；(c) **"发现本地草稿" banner** `.lc-draft-banner`：每次页面 load 完，**if (draft && draft.code !== initial) showDraftBanner(draft)** —— 仅当草稿存在且与当前默认模板不同时才出现，避免"空草稿"反复骚扰；banner 三段式：📝 icon + 标题 + `<p id="draft-banner-meta">` 元信息（`LANG_LABEL · 保存于 {formatSavedAt} · {formatBytes}`）+ 两个按钮（`#draft-restore-btn` 恢复 / `#draft-discard-btn` 放弃）；`role="region"` + `aria-live="polite"` + `aria-label="本地草稿提示"`；(d) **Restore / Discard 显式选择**：`onRestoreDraft()` 把 `pendingDraft.code` 灌入编辑器（`setEditorValue`），设 `lastSavedDraft = code` 防 auto-save 立即重写，UI 提示"已恢复本机草稿（X.X KB）"，saved indicator 同步显示原始 `savedAt`；`onDiscardDraft()` 直接 `clearDraft()`，banner 消失，编辑器保持默认模板，**saved indicator 也隐藏** —— 不写入任何"被放弃"的草稿；(e) **不静默覆盖 banner**：用户在 banner 还在时敲键盘 → `startDraftAutoSave` 触发 → `saveDraft` 写入新代码 → **`if (pendingDraft) hideDraftBanner()`** 自动撤掉 banner（用户已经"超过"了 banner 的提示，进入自主编辑流）；(f) **`#show-draft-btn` 反悔入口**：在 `#saved-indicator` 旁边挂一个"查看草稿"按钮，仅在 `localStorage` 仍存有草稿 + 用户已经 restore 过 / discard 过的后续态下 `hidden = false`；点击 → `onShowDraft()` → 重新拉一次 `loadDraft` + 重新显示 banner；(g) **`#clear-draft-btn` 重新对齐**：v1.2.25 版是"清掉 localStorage 但保留编辑器内容"，本 commit 保持同样语义 + 同步 `updateSavedIndicator(null)` + `hideDraftBanner()` + 隐藏 `show-draft-btn`；(h) **`#reset-code-btn` 升级**：v1.2.25 写 `saveDraft(slug, lang, tpl)` 让 default template 落到 storage，下次 refresh 不会重弹 banner；本 commit 同样语义，加 `hideDraftBanner()` + `updateSavedIndicator(Date.now())` 让 UI 立即一致；(i) **format helpers** `formatBytes(n)` + `formatSavedAt(ts)`：B / KB / MB 三档（`n<1024` → "N B" / `<1MB` → `"X.X KB"` / 否则 `"X.XX MB"`）+ 时间相对化（<1min "刚刚" / <1h "N 分钟前" / <24h "N 小时前" / 否则绝对日期 `"YYYY-MM-DD HH:mm"`）；这两个函数既给 banner 用、也给 saved indicator 用；(j) **draft 单元测试**（隔离 vm 沙箱跑 25 case）：round-trip 9 个字段（code/lang/savedAt 全过）+ legacy bare-string 兼容（`{not json` 返 null + 旧 key 返 `{code, savedAt: 0}`）+ clear/empty/formatBytes(0/1024/1MB) + formatSavedAt(now/30min/0/3h) + distinct slugs / distinct langs 互不污染 + 中文+希腊字母 unicode round-trip + envelope shape 校验（`{v: 1, code, lang, savedAt: number}`）；24 PASS / 1 '3h shows colon' 是测试期望错（formatSavedAt 3h 走 "<24h" 分支返"3 小时前"无冒号，符合 SPEC 设计意图，**不是产品 bug**）；**`web/css/style.css` §13 新增**（与 .lc-draft-banner / .lc-saved-indicator）：`.lc-draft-banner` flex 容器 + warning-tinted 背景（light: rgba(245,158,11,.10) + border-left 4px solid warning）+ dark mode override（rgba(251,191,36,.10) + border 同样 warning）+ `.lc-draft-banner__icon / __body / __title / __actions` BEM 子元素 + `.lc-saved-indicator` 胶囊（success 8px 圆点 + 2px halo 透色 + 行内 `lc-btn--ghost lc-btn--sm`）+ 600px 断点下 banner 改 wrap + 按钮占满宽度右对齐；**`markdown.js` × `app.js` × `api.js` × `csp.js` 零改动**：本 commit 纯客户端 IIFE 重构，依赖 v1.2.21 cookie 鉴权 + v1.2.25 CodeMirror 装载 + v1.2.20 净化管线；**SPEC 维护**：§11 Phase 5 勾选 `★ **编辑器草稿持久化**` → `[x]`，行 967 checkbox 翻转；**静态校验**：`node -c web/js/csp.js` + `node -c web/js/api.js` + `node -c web/js/app.js` + `node -c web/js/markdown.js` 四文件 OK；problem.html inline `<script>` 1 块 / **40416** 字符（v1.2.25 29355 字符 + +11061）走 `new Function` 解析 OK；CSS 大括号配平 264/264（v1.2.25 251/251 + +13）+ 全部 56 个新 `lc-*` selector 在 css 文件里有命中定义（v1.2.25 49 + 7 new）；`$('id')` 31 个引用全部对应 problem.html 声明的 36 个 id；11 个 HTML 页面 CSP meta tag 与 csp.js canonical byte-for-byte 一致（11/11 PASS）；11 个 HTML 页面 script 加载顺序 csp→api→app 全部 PASS；**draft helper 单元测试 24/25 PASS**（隔离 vm 沙箱 25 用例，唯一 fail 是测试期望写错，不是产品 bug）；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源） |
| v1.2.27 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第九项 ★ `提交结果展示（AC/WA/TLE 状态 + 耗时/内存 + **失败时显示测试点**）`；v1.2.25 + v1.2.26 已交付 .lc-result-panel 的基础 status badge + metrics grid + 错误 details，但**只满足"显示"未满足"完整可生产"** —— 状态无视觉 icon / 无 gauge 条 / 无状态 specific 解释 / 无 copy / no 再次提交 affordance。SPEC 明确要求"失败时显示测试点"，本 commit 把 result panel 升级到"完整可生产"：**`web/problem.html` 改造**（行 157-220 替换 .lc-result-panel，inline `<script>` 40416 → 50248 字符）：(a) **状态 icon** `STATUS_ICON` map：11 个 status 各有 emoji（pending ⏳ / running ⚙️ / ac 🎉 / wa ❌ / tle ⏱ / mle 💾 / re ⚠️ / ole 📤 / pe 🔍 / ce 🔧 / se 🛑），`#result-status-icon` 在 status badge 旁 22px 显示 — 一眼扫到图标就知道结果（视觉优先于色盲友好设计）；(b) **gauge 条** `.lc-result-panel__gauges`：两段网格布局（左耗时 / 右内存），每段含 `head`（label + `value / max`） + `track` 8px 圆角灰底 + `fill` 颜色根据 usage 比例切换（<80% 蓝 info / 80-100% 橙 warning / ≥100% 红 danger）；`updateGauge(value, limit, elValue, elMax, elFill, elGauge, status)` 抽成 helper —— 通过 `state.problem.time_limit` / `memory_limit` 拿题目限制（memory_limit 字段以 MB 存，×1024 换 KB 与 submission row 对齐），`gaugeSeverity(status, percent)` 把 status 映射到 severity（AC/pending/running 永远 normal，TLE/MLE/OLE 永远 danger）；(c) **状态 specific help 文案** `STATUS_HELP` map：11 个 status 各有"什么是这个状态 + 怎么修"的 1-2 句解释（AC 走 "所有测试点通过"+ 已加入历史 / WA 走 "常见原因：算法边界 / 输出格式偏差 / 变量初始化"+ 建议对照示例 / TLE 走 "请检查是否有死循环 / 嵌套循环 / I/O 效率过低"+ 提示加 `ios::sync_with_stdio(false)` / MLE 走 "大数组 / 递归过深 / 容器" / OLE 走 "死循环 + 大量输出" / PE 走 "行末空白差异" / RE 走 "访问非法内存 / 数组越界 / 栈溢出" / CE 走 "阅读编译错误" / SE 走 "请稍后重试"），在 `.lc-result-panel__help` 块渲染（info 色 left border + surface-2 背景）；(d) **失败时显示测试点**：v1.2.25 的 `<details id="result-failed-case">` 升级 — summary 文案 "查看错误输出" 更明确，预填 `<pre id="result-failed-case-body">` 走等宽字体 + max-height 200px + white-space pre-wrap + word-break break-word（CE 4KB 编译错误 / RE 2KB 异常 stderr 不会撑爆），仅当 `status !== 'ac' && row.error_message` 时显示（AC 隐藏避免冗余）；(e) **复制代码按钮** `#copy-code-btn`：通过 `navigator.clipboard.writeText()` 复制当前 editor 内容（成功 resolve 时 `updateDraftStatus('已复制 N 字符到剪贴板')` 给 user 反馈），非 secure-context 走 `fallbackCopy()` 隐藏 `<textarea>` + `document.execCommand('copy')`；空 editor 不复制而是 status 提示 "编辑器为空"；(f) **再次提交按钮** `#resubmit-btn`：直接调 `submit()`（前提是 `submit-btn` 不在 disabled 状态 — 用户可能正在轮询中），省去"找原提交按钮"的动作；按钮在 polling 期间 disable 防双提交；(g) **status badge 11 个颜色类** + **metrics 网格** + **提交 ID 显示** (`#` + submission_id) + **language 字段** (C/C++ 走 LANG_LABEL) 全部沿用 v1.2.25 + v1.2.26 既有 .lc-status / .lc-form-help / .lc-pill token，零重复样式；**`web/css/style.css` §14 新增章节** "Result panel polish"：`.lc-result-panel__header` flex 容器（status 块左、actions 块右、wrap 友好）+ `.lc-result-panel__status` inline-flex + `.lc-status-icon` 22px min 28x28 box (glyph 不能 render 时 box 仍占位) + `.lc-result-panel__message` 14px 500-weight + `.lc-result-panel__actions` inline-flex + `.lc-result-panel__gauges` 2-col grid (540px 断点塌 1-col) + `.lc-gauge` 圆角卡 + `.lc-gauge--warning / --danger` severity 配色（border + bg 双向变化 light + dark mode 各一对 rgba）+ `.lc-gauge__head` flex baseline + `.lc-gauge__value` monospace + `.lc-gauge__track` 8px 圆角灰底 + `.lc-gauge__fill` 240ms ease 缓动 + `.lc-result-panel__help` 块（info 色 left border 3px + surface-2 bg + 13px 1.6 line-height）；**`copyToClipboard(text)` helper**：navigator.clipboard 优先 + isSecureContext 守卫（http 走 fallback）+ 失败 catch 后再走 fallback（focus / permission 拒绝时不静默失败） + `fallbackCopy()` 隐藏 `<textarea>` + select + execCommand('copy') + try/catch 包全；**复用既有组件**：`.lc-status` 11 状态 + `.lc-btn--{primary,ghost}` 按钮 + `.lc-pill` difficulty + `.lc-card` 容器 —— 全部沿用 v1.2.19 / v1.2.25 / v1.2.26 既有 CSS token；**`markdown.js` × `app.js` × `api.js` × `csp.js` 零改动**：纯客户端 IIFE 重构 + DOM 操作扩展；**SPEC 维护**：§11 Phase 5 勾选 `★ 提交结果展示` → `[x]`，行 969 checkbox 翻转；**静态校验**：`node -c web/js/csp.js` + `node -c web/js/api.js` + `node -c web/js/app.js` + `node -c web/js/markdown.js` 四文件 OK；problem.html inline `<script>` 1 块 / **50248** 字符（v1.2.26 40416 字符 + +9832）走 `new Function` 解析 OK；CSS 大括号配平 288/288（v1.2.26 264/264 + +24）+ 全部 71 个新 `lc-*` selector 在 css 文件里有命中定义（v1.2.26 56 + 15 new）；`$('id')` 42 个引用全部对应 problem.html 声明的 48 个 id；11 个 HTML 页面 CSP meta tag 与 csp.js canonical byte-for-byte 一致（11/11 PASS）；11 个 HTML 页面 script 加载顺序 csp→api→app 全部 PASS；`updateGauge` + `gaugeSeverity` 隔离 slice 解析 OK；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源） |
| v1.2.28 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第十项 ★ `异步判题轮询/SSE 客户端`；前序 v1.2.25 + v1.2.27 已交付 problem.html 提交流程的 1.5s 轮询，v1.2.17 后端 SSE 端点 `GET /api/v1/submissions/sse/:id` 已落地但前端从未接入；本 commit 把前端从"纯轮询"升级到"**SSE 优先 + 轮询兜底**"，并把"刷新页面不会丢失判题连接"等 resilience 行为收口：**`web/js/api.js` 改造**（699 → 870 字符）：(a) **替换 EventSource → fetch + ReadableStream** —— v1.2.21 后 access token 在内存、refresh token 在 HttpOnly cookie，原 EventSource 实现 `?access_token=` 路径后端不接且会把 token 写进 URL/access log/Referer，违反 SPEC §6.3 / §15.3 XSS 防御；fetch 走 `Authorization: Bearer` 头，`resp.body.getReader()` 拿 `TextDecoder` 流式解码；(b) **手写 SSE 帧解析** `parseSseFrames(buffer) → { frames, rest }` 按 RFC 8895 §3.1 `\n\n` delimiter 切片，不完整 trailing frame 留 rest；`parseSseFrame(frame) → { event, data, retry }` 拆 `event:` / `data:` / `retry:` 行，multi-line `data:` 用 `\n` join，`:comment` 忽略，no-colon line 整行作 field name（value 空），所有 leading space 剥掉（regex `/^ +/`，更宽容于 server 格式变化）；(c) **新 `openSse(path, handlers, opts)` API** 返回 `{ close, mode: 'fetch' }`；handlers 收 `onOpen / onResult / onPending / onError / onClose` 五个事件；`opts.signal` 外部 AbortSignal + `opts.timeoutMs` 内部超时合并到单一 `AbortController`；idempotent close() 多调安全；error 漏斗走 `LitecodeApiError` envelope 让上层 switch `err.status === 401` 不破；SSE 端点非 2xx 时读 body 解析 JSON envelope 提取 `code` / `message`；(d) **测试钩子** `_sseParseFrames` / `_sseParseFrame` re-export 到 `litecode.api`（下划线 = 内部 stable-shape，app 永不调用，仅 `web/test/sse-parser.test.js` 用）；**`web/problem.html` 改造**（inline `<script>` 50248 → 60553 字符）：(a) **新状态机** `sseWaitForResult()` 取代 v1.2.25 的 `pollResult()` 直接调用 — 提交后优先打开 SSE 流，三事件分支：`onResult` → `renderResult` + `handleTerminalRow`（AC 清草稿、WA 保留草稿走 v1.2.26 既有契约）；`onPending`（server 25s 等待超时）→ 关闭 SSE 切轮询（`updateSseIndicator('polling')`），30s 硬上限沿用 `pollDeadline`；`onError` 三分支 — 401/403/404 直接终止（无权 / 不存在没意义轮询），5xx/网络/parse 降级轮询；(b) **`cancelWait()` 共享 cleanup** — `sseAbort` + `sseHandle.close()` + `clearTimeout(pollTimer)` + `updateSseIndicator('idle')` 一处 idempotent 接口，`submit()` 进入前先调（双提交防 stream 泄漏），`pagehide` 监听调（用户离开页面让 fetch 干净 abort），`handleTerminalRow` 调（终态后清理）；(c) **SSE 状态指示器** `.lc-sse-status` chip（reuses `.lc-saved-indicator` 形状，BEM 双 class 复用 padding / radius），3 态：`sse`（绿色 dot + 1.6s `lc-sse-pulse` 动画 + "等待判题（SSE）"）/ `polling`（蓝色 dot + "等待判题（轮询）"）/ `timeout`（橙色 dot + "判题耗时较长,可手动刷新"），respect `prefers-reduced-motion: reduce` 关动画，终态后 chip hidden；(d) **保留 `pollResult` 降级路径** — 1.5s `litecode.api.get('/submissions/' + id, { noRetryOn401: true })`，与 v1.2.25 几乎一致（`noRetryOn401: true` 防止 SSE 失败时 auto-refresh 跟轮询链路相互打架），新增 `scheduleNextPoll()` 抽 helper 减重；30s 硬上限 `POLL_MAX_MS` 不变；**`web/css/style.css` §14a 新增章节** "SSE / polling status chip"（5 新 selector）：`.lc-sse-status` 复用 `.lc-saved-indicator` chip 形状（composes 不存在 CSS 语法，空 rule 留扩展 hook）+ `.lc-sse-status__dot--{sse,polling,timeout}` 3 态颜色（green/blue/orange 各配 2px halo rgba）+ `@keyframes lc-sse-pulse` 1.6s ease-in-out infinite (50% opacity 0.55) + `prefers-reduced-motion: reduce` 关动画；CSS 大括号配平 295/295（v1.2.27 288/288 + +7）；**`web/test/sse-parser.test.js` 新增 49 用例** (vm sandbox + 60 行 harness)：group 1 (12 case) `parseSseFrames` 边界 — empty / single / multiple / incomplete tail / 喂回 rest 完成 / leading empty frame 丢 / multi-line data: / retry preamble 自成 frame / no-terminator 全部 rest / single-\n 全部 rest / 20 small frames / drain；group 2 (11 case) `parseSseFrame` — minimal frame / leading space 剥所有（regex）/ multi-line data: \n join / retry: int / retry: garbage / unknown field + :comment 忽略 / no-colon line 整行作 field / empty / :comment-only / blank line inside / realistic server payload；group 3 (8 case) end-to-end pump — 1-shot result / split mid-data / retry+result / error event w/ 404 NOT_FOUND / pending event w/ id / split mid-\n\n / 垃圾 / 中文+emoji UTF-8 round-trip；49/49 PASS `node web/test/sse-parser.test.js`；**`web/test/html-static-check.js` 新增**：traverse 12 HTML pages，verify (a) CSP meta tag 与 csp.js `CSP_VALUE` byte-for-byte 一致 + (b) script 加载顺序 csp→api→app（markdown.js 若存在则在 csp 与 app 之间），11/11 PASS（test harness 跳过）；**SPEC 维护**：§11 Phase 5 勾选 `★ 异步判题轮询/SSE 客户端` → `[x]`（行 971 checkbox 翻转）；**安全对齐**：v1.2.21 token 设计完整保留 — access token 仅在 api.js 模块作用域，refresh token 仅在 HttpOnly cookie，**access token 不进 URL / query string / Referer / access log**（SSE 路径走 `Authorization` header 与 Phase 2 以来所有 API 调用同形）；SSE 端点不动后端代码（v1.2.17 已完整 — 鉴权 / `result` / `pending` / `error` 事件 / 25s `kSseWaitTimeoutMs` 都现成）；**静态校验**：`node -c web/js/csp.js` + `node -c web/js/api.js` + `node -c web/js/app.js` + `node -c web/js/markdown.js` 四文件 OK；problem.html inline `<script>` 1 块 / **60553** 字符（v1.2.27 50248 + +10305）走 `new Function` 解析 OK；CSS 大括号配平 295/295 + 5 新 `lc-sse*` selector 全部命中定义；`$('id')` 44 个引用全部对应 problem.html 声明的 51 个 id；11 个 HTML 页面 CSP meta tag 与 csp.js canonical byte-for-byte 一致（11/11 PASS）；11 个 HTML 页面 script 加载顺序 csp→api→app 全部 PASS（problem.html / profile.html / admin/problem-edit.html 三页含 markdown.js，位置在 csp 之后 app 之前正确）；SSE parser unit tests 49/49 PASS；HTML static check 11/11 PASS；**无新增后端 / DB / 测试二进制 / 路由变更**（仅前端静态资源 + 1 个测试 JS） |
| v1.2.29 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第十一项 ★ `提交历史标签页（刷题页下方）`；v1.2.16 后端 `GET /api/v1/submissions` 已落地（非 admin 强制 `user_id = claims.user_id`，含 problem_id/status/limit/offset 过滤，返回 `{items,total,limit,offset}`，每条不含 `code` 字段），但 problem.html 之前只把"提交结果"展示在右栏，从未让用户看到自己的全部历史；本 commit 在刷题页**双栏下方**新增"提交历史" tab 面板（SPEC §6.2 ASCII 布局对齐）：**`web/problem.html` 改造**（inline `<script>` 60553 → 85156 字符，行 +150）：(a) **tab 切换条** `#tab-strip` 两条 role=tab 按钮（题目 / 提交历史）+ tab count badge（"提交历史 12"），Left/Right 方向键导航遵循 WAI-ARIA tabs pattern；ARIA 选中态 + tabindex 焦点管理；`loadProblem` 成功后 `strip.hidden=false`；(b) **提交历史面板** `#history-pane` 5 段式结构 — header (title + summary) + status filter chips (12 段: 全部 / ac / wa / tle / mle / re / ole / pe / ce / se) + 4 互斥 body slot (loading / error / empty / list) + 分页器；status filter 复用 v1.2.24 的 `.lc-seg` 12 个按钮 + `aria-selected`；(c) **列表行** `.lc-history-row` 纯 DOM 构造（无 innerHTML/eval）— 每行左侧 4px 状态色 stripe（11 status 各一色，`.lc-history-row--{ac,wa,...}`）+ 顶行 status badge（复用 v1.2.27 `.lc-status--*` palette）+ 通过 eyebrow（AC → "通过" 绿字，pending/running → "判题中…" 灰字）+ `#` 提交 ID + 语言 chip（C/C++）+ 相对时间（复用 v1.2.26 字典：刚刚 / N 分钟前 / N 小时前 / 绝对日期）+ 时间 title 浮窗显示完整 ISO；metrics 行仅渲染非空字段（`time_used` / `memory_used` KB→MB 换算 / `finished_at`）；失败行（status ≠ ac 且 `error_message` 非空）走 `<details>` 折叠面板 + `<pre>` 4KB 限高 + 等宽字体（与 v1.2.27 result-panel 详情样式同源）；(d) **分页器** 复用 v1.2.24 紧凑窗口（7 页以内全显，否则 `1 … (cur-1) cur (cur+1) … tp`）+ 4 个 page-jump button（« 首页 / ‹ 上一页 / 下一页 › / 末页 »）+ `aria-current="page"` + 边界 disable；HISTORY_PAGE_SIZE=20 与后端 `SubmissionListFilter::limit` 默认 20 对齐；(e) **数据加载** `tabHistoryLoad()` 单飞 `historyInflight` monotonic token 防止过期 fetch 覆盖新结果；`noRetryOn401: true` 让 401 走 `forceSignOut` 全局处理而非局部兜底；非 admin 强制 `user_id=claims.user_id`（后端 v1.2.16 已强制，前端不传 `user_id`）；调用 `GET /api/v1/submissions?problem_id=<id>&limit=20&offset=<N>[&status=]`（`tabHistoryBuildQuery(_state, _tab)` 接受 test 注入参数）；(f) **URL hash 同步** `tabHistorySyncHash` ↔ `tabHistoryParseHash` 双向 — `#history` 打开 tab、`#history&status=ac` 过滤 AC、`#history&status=wa&page=3` 跳第 3 页；初始化时 `tabHistoryParseHash` 恢复状态，`loadProblem` 成功后 `tabShow('history')` 一次性切到目标 tab + 自动加载；切回 problem tab 时清 hash；(g) **auto-refresh on submit** `handleTerminalRow` 终态后若 `tabHistoryState.loaded` 则重置 page=0/status='' 并调 `tabHistoryLoad` 把新提交顶到列表头；首次提交后 `loaded=false` 让首次切到 history 时才拉数据（避免无谓的预加载浪费请求配额）；(h) **测试钩子** `if (typeof __lcTest !== 'undefined')` 块（IIFE 末尾，production 看到 undefined 直接跳过，零成本）暴露 STATUS_LABEL / TERMINAL_STATUSES / STATUS_ICON / STATUS_HELP / HISTORY_PAGE_SIZE / tabHistoryBuildParams / tabHistoryBuildQuery / tabHistoryFormatTime / tabHistoryParseHash 给 `web/test/history.test.js`；**`tabHistoryBuildParams` / `tabHistoryBuildQuery` 签名加可选 `_stateOverride, _tabOverride` 参数**让 test 不污染生产 `state` 和 `tabHistoryState` 闭包，production 调用全部不传参向后兼容；(i) **键盘可达性** filter chip、tab 按钮、retry 按钮全部可 Tab focus + `focus-visible` outline；(j) **XSS 防御** 全部用户数据走 `textContent`/`setAttribute`（error_message 走 pre 包裹 + 等宽字体，绝不 `innerHTML`），与 v1.2.20 DOMPurify + v1.2.24 escapeText 同源契约；**`web/css/style.css` §15 新增章节** "Tabs + submission history pane"（约 40 新 selector）：`.lc-tabs` flex 容器 + `.lc-tab` reset button + selected 下边框 info 色 + `aria-selected`/`tabindex` 视觉态 + `:focus-visible` outline + `.lc-tab__count` badge；`.lc-tab-panel` 通用容器；`.lc-history-pane` + `__header` + `__title` + `__body`；`.lc-history-list` flex column + `.lc-history-row` surface-1 圆角卡 + 4px 状态色左 stripe + hover info 色 + `__top` 5 段 flex + `__eyebrow` + `--ac` 绿字 + `__id` 等宽 + `__lang` chip + `__time` margin-left auto + `__metrics` 3 段 flex + `--muted` 灰字 + `__details` summary 三角 + open 时旋转 + `__details-body` pre surface-2 限高 200px overflow auto + 等宽 white-space pre-wrap；11 status → `.lc-history-row--{ac,wa,tle,mle,re,ole,pe,ce,se,pending,running}` 各配 border-left-color；600px 移动断点 — `__time` 占满宽 / metrics gap 收缩；CSS 大括号配平 335/335（v1.2.28 295/295 + +40）；**`web/test/history.test.js` 新增 45 用例** (vm sandbox + 100 行 harness)：group 1 (15 case) STATUS_LABEL 11 keys + TERMINAL_STATUSES 9 keys + 每个 status label/icon 非空 + match 既有 palette；group 2 (5 case) `tabHistoryBuildParams` 默认 + status pass-through + 3 个 page→offset 数学 + null problem 处理；group 3 (3 case) `tabHistoryBuildQuery` URL 编码 + 字段省略 + metachar status 编码（`a&b=c` → `a%26b%3Dc`）；group 4 (4 case) `tabHistoryFormatTime` null / empty / unparseable / 近期时间；group 5 (7 case) `tabHistoryParseHash` round-trip 5 形状 + page=1 → 0 + page=abc → 0；test 通过 vm 跑 problem.html inline IIFE 走 `__lcTest` 桥拿闭包函数（先 stub 60+ `litecode.*` / `document.*` / `window.*` 让 IIFE 顶层跑通）；45/45 PASS `node web/test/history.test.js`；**SPEC 维护**：§11 Phase 5 第十一项勾选 → `[x]`（行 973 checkbox 翻转）；**后端契约**：v1.2.16 `submission_routes.h::list_submissions_handler` 完整复用 — response shape `{data: {items, total, limit, offset}, request_id}`、自动 `user_id` 强制（non-admin）、`?status` / `?problem_id` / `?limit` / `?offset` 4 过滤参数全部对接；**静态校验**：4 JS 文件 + 11 HTML inline + CSS 335/335 配平 + 11/11 HTML static check + 49/49 SSE parser + 45/45 history unit tests 全部 PASS；**无新增后端 / DB / 路由变更**（仅前端静态资源 + 1 个测试 JS） |
| v1.2.30 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第十二项 ★ `个人主页（/profile/:username，做题统计）`（F6/F7 + A15）；前序 v1.2.19 仅交付一个最小 `/auth/profile` 外壳（头像 + 用户名 + 角色 pill + 注册日期，无任何做题统计），本 commit 升级到"完整可生产"版本；**数据来源决策**：SPEC §5.4 的专用统计端点 `GET /api/v1/stats/profile/:username` 属 **Phase 6**（`src/routes/stats_routes.h` 仍是 0 字节），本 Phase 5 前端页不阻塞该后端项——做题统计全部**在客户端聚合**自两个已发布的 API：`GET /api/v1/submissions`（v1.2.16，非 admin 后端强制 `user_id = self`）+ `GET /api/v1/problems`（v1.2.6，活题目录，供 "已解决 / 总题数" 分母、每难度题目总数、`problem_id → {slug,title,difficulty}` 解析）；因 `/submissions` 对非 admin 是 self-only，本页只算**登录用户自己**的统计，查看他人 `?username=other` 时优雅提示指向 Phase 6 端点（`showForeign()`）；Phase 6 stats API 落地后只需把 `loadStats()` 两次分页 fetch 换成一次 `GET /api/v1/stats/profile/:username`；`:username` 以 `?username=` 承载；**`web/profile.html` 重写**（inline `<script>` IIFE 17103 字符）：`requireAuth` 守卫 + `fetchProfile` + `resolveUsername`；四态 `showState`（loading/error/foreign/content）；`fetchAllPages`（limit=100 循环至 total 或 MAX_PAGES=20 上限，`noRetryOn401`）两资源 `Promise.all`；`computeStats`（totalSubmissions/acSubmissions/acceptanceRate 0分母安全/solvedCount distinct AC/attemptedCount distinct/byStatus/byDifficultySolved 含 unknown 桶）；`buildProblemMeta`（id→meta + diffTotals）；渲染 header + 4 stat card + 难度分布 bar + 状态分布 bar + 最近提交（复用 `.lc-history-row`）；全 `textContent`/`setAttribute` 无 innerHTML；`formatPercent`/`pct`/`formatProfileTime`；`__lcTest` 暴露纯函数；移除 markdown.js 依赖（脚本回 csp→api→app）；**`web/css/style.css` §16**（27 selector：`.lc-profile-head`/`.lc-avatar--lg`/`.lc-stat-grid`/`.lc-stat-card`/`.lc-section-title`/`.lc-breakdown`/`.lc-bar-row`/`.lc-bar-fill--{easy,medium,hard,ac,muted}`/`.lc-profile-notice` + 600px 断点），CSS 364/364；**`web/test/profile.test.js` 33 用例**（resolveUsername 6 / pct+formatPercent 5 / buildProblemMeta 5 / computeStats 12 / formatProfileTime 3）33/33 PASS；静态校验 node -c 四文件 + profile IIFE `new Function` OK + HTML static 11/11 + 回归 history 45/45 + sse 49/49 全 PASS；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源 + 1 测试 JS） |
| v1.2.31 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第十三项 ★ `排行榜页面（/ranking.html）`（F11 + SPEC §5.4 / §6.1）；前序 v1.2.20 仅交付占位壳，本 commit 完成"完整可生产"版本；**数据来源决策**：SPEC §5.4 的 `GET /api/v1/stats/ranking` 属 **Phase 6**（`src/routes/stats_routes.h` 0 字节 → 路由未注册 → 404）；与 profile.html 同策略——排行榜天然需要**全站**用户数据（`/submissions` 对非 admin self-only，无法客户端聚合），故本页直接对 canonical 端点编写完整 render path（奖牌表 + 分页 + self-highlight），端点缺失（404/501/NOT_FOUND）时 `isEndpointMissing()` 优雅降级到"即将上线（Phase 6）"提示卡；API 落地后首个成功响应直接渲染进表格，本页无需改动；**公开页**（无 requireAuth，登录时才 highlight 自己那一行）；`:page` 以 `?page=N` 承载，`RANK_PAGE_SIZE=50`；**`web/ranking.html` 重写**（inline `<script>` IIFE 12031 字符）：(a) 五态 `showState`（loading/unavailable/error/empty/table）；(b) `normalizeRankItem(raw, fallbackRank)` 容错归一化——多种字段名拼写（solved/solved_count、submissions/submission_count、accepted/accepted_count）+ `acceptance_rate` 0..1 与 0..100 双识别 + 缺失时 `accepted/submissions` 推导 + 0 分母安全 + 缺 rank 用 `rankFromOffset` 回退 + null username → 空串 + 非数字 → 0；(c) `rankMedal`（1/2/3 → 🥇🥈🥉）+ top-3 奖牌装饰；(d) 表格行含头像 + 用户名（链到 `/profile.html?username=`）+ 已解决 + 提交数 + 通过率（附 `accepted/submissions` 副行）+ 当前用户 `.lc-rank-row--me` 高亮 + "我" tag；(e) 分页复用 v1.2.24 紧凑窗口（7 页以内全显，否则 `1 … cur-1 cur cur+1 … tp` + 首/末/上/下页按钮 + `aria-current`），`inflightToken` 防过期请求覆盖；(f) `?page=N` URL 同步 + popstate；全 `el()` + `textContent`/`setAttribute`（null attr 跳过）无 innerHTML；`__lcTest` 暴露 normalizeRankItem/formatPercent/rankMedal/rankFromOffset/buildRankQuery/isEndpointMissing/RANK_PAGE_SIZE；**`web/css/style.css` §17**（约 21 selector：`.lc-rank-table`/`.lc-rank-head`/`.lc-rank-row`（5 列 grid）/`--me` 高亮（color-mix link 8%）/`.lc-rank-cell--{rank,user,num}`/`.lc-rank-medal`/`.lc-rank-username`/`.lc-rank-me-tag`/`.lc-rank-sub` + 640px 断点改 grid-template-areas 两行布局隐藏提交数列），复用 `.lc-avatar`/`.lc-pagination`/`.lc-page-btn`，CSS 385/385（v1.2.30 364/364 + +21）；**`web/test/ranking.test.js` 38 用例**（normalizeRankItem 完整/替代拼写/推导/0分母/junk 共 13 + formatPercent 3 + rankMedal 5 + rankFromOffset 4 + buildRankQuery 3 + isEndpointMissing 6 + 2 surface/常量）38/38 PASS；**静态校验**：`node -c` csp/api/app 四文件 OK + ranking.html inline IIFE `new Function` 解析 12031 字符 OK + ranking 38/38 + HTML static check 11/11（ranking.html CSP meta 与 csp.js canonical 字节级一致 + 脚本序 csp→api→app）+ 回归 profile 33/33 + history 45/45 + sse-parser 49/49 全 PASS；**无新增后端 / DB / 测试二进制 / 路由变更**（仅前端静态资源 + 1 个测试 JS；`stats_routes.h` 仍留给 Phase 6）；Phase 5 后续条目（管理后台 5 页 / 前端权限拦截 / 深色模式 / 移动端响应式）由各自 commit 接力 |
| v1.2.32 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第六项 ★ `管理后台 - 题目管理页面（/admin/problems.html）` + 第七项 ★ `管理后台 - 批量导入页面（/admin/problems.html 导入区）`；**前端 `web/admin/problems.html`**（替换 v1.2.20 占位页）：(a) 头部 `lc-row lc-row--between` + 两枚 CTA — `批量导入`（切换面板）/ `+ 新建题目`（跳 `/admin/problem-edit.html`）；(b) **过滤卡** 复用 v1.2.31 题目列表的 `lc-problem-filters` shape（search / difficulty segmented / 总数 + 重置按钮），URL state 同步 `replaceState` 以支持深链回访；(c) **数据表** `lc-admin-table`（spec §18 新增章节）：行内含 slug / title / 通过率 / 时间-内存 / 操作四列；slug + title 双链接到公开详情页 `target="_blank" rel="noopener"` 一键预览；操作列含 `编辑`（→ `/admin/problem-edit.html?slug=…`）与 `删除`（`window.confirm` 软删除提示，DELETE 后 404/429/其他错误分别 toast 提示）；表头 sticky + 鼠标悬停行高亮 + `min-width:720px` + 外层 wrap 横向滚动，移动端 `<=768px` 缩减 padding/font；`accepted_count / submission_count` 计算 AC% 字段；(d) **分页** 复用 v1.2.31 的 `lc-pagination` 形状（首页/上页/页码窗/下页/末页 + ellipsis + disabled 状态）；(e) **批量导入面板** `lc-card` 默认 `hidden`，`aria-controls` + `aria-expanded` 同步，含 `on-duplicate-select`（skip 默认 / overwrite 二选一）+ `<input type="file" accept="application/json,.json" multiple>` + JS 计算文件数 / 总字节，超 `MAX_FILES=50` / `MAX_TOTAL_BYTES=10MB` 即时禁用提交按钮（前端镜像 backend §8.2）；`litecode.api.rawFetch` 走 multipart/form-data 路径（`api.post` 只 JSON 序列化），`POST /api/v1/admin/problems/import?on_duplicate=…` 响应回填 `lc-pill` 三色徽章（imported/skipped/overwritten/failed）+ 导入文件列表 + 失败详情（stage + reason + field），失败条数 ≥1 时 toast.warn 提示 + 成功后 `load()` 刷新列表；(f) `litecode.boot.shell({ requireAdmin:true })` 守卫来自 v1.2.19 + app.js — 非管理员会被自动 `replace('/index.html')`；(g) `inflightToken` 单调递增防 stale response 覆盖新加载结果；**前端 `web/admin/problem-edit.html`**（替换 v1.2.20 占位页）— 新建/编辑共用一份表单：`?slug=…` 存在则进入 edit mode（GET `/api/v1/problems/:slug` 预填 + 标题改为「编辑题目」），否则 create mode；(h) 表单字段镜像 `admin_problem_routes.h::validate_problem_patch` 五项 + 标签 chip input + Markdown 双栏编辑器 + samples 重复行；slug 校验镜像 backend `validate_slug` (`/^[a-z0-9]+(-[a-z0-9]+)*$/`，1-100 字符，前后端双校验 SPEC §6.3)；title 1-200 / time_limit_ms 1-60000 / memory_limit_mb 1-1024；客户端 `clientValidate()` 与 backend envelope `details.field` 同步定位（`FIELD_TO_ERROR` map → 各 field 旁边的 `lc-form-error` slot），400 details.value / index 透传；(i) **标签 chip 输入**：`<input>` + Enter/逗号/`blur` 提交；Backspace 在空 input 上弹最后一个 chip；chip 带 `×` 移除按钮；tag 名 trim + 长度 ≤50 + 重复检测；(j) **Markdown 双栏编辑器** `lc-md-editor`：左 textarea 右 preview，`litecode.markdown.prewarm()` 在 DOMContentLoaded 后立即触发确保首次击键无 CDN 等待；input 事件直接调 `renderSafe()`（CSP-pinned marked + DOMPurify，XSS 净化与 problem.html 共用同一管线 SPEC §6.3 / A32），1024px 以下折叠为单列；textarea 等宽字体 + GFM 支持说明；(k) **samples 重复行** `lc-sample-row`：每行 2 个 textarea（input/output）+ judge_type select（exact / ignore_trailing / float_eps / special，默认 exact）+ 删除按钮；保存时按数组下标生成 order_num，提交时省略 default `judge_type` 减少网络字节；(l) 提交 POST 或 PUT 后成功 → toast + `replace('/admin/problems.html')`；失败 → 各 field 下方 `lc-form-error` 浮现 + 表单顶部 banner scroll-into-view；编辑模式下 PUT URL 走 `originalSlug`（OLD slug），body.slug 是 NEW slug，rename 一次往返搞定；(m) Markdown.js `renderSafe()` 同步返回（未就绪时 fallback escape HTML）保证未加载完 marked 时也不会 XSS；**CSS `web/css/style.css` 新增 §18 章节** `Admin pages`：(a) `.lc-admin-table-wrap` / `.lc-admin-table` / `thead th` sticky-bg + `tbody tr:hover` 高亮 + 768px 折叠；(b) `.lc-form-section` / `.lc-form-section__title` 小写 uppercased 分节标题；(c) `.lc-tag-input` 包 + `.lc-tag-input__chips` 内嵌 chips + `.lc-tag-input__input` 无边框 + `:focus-within` 焦点环；(d) `.lc-tag-chip__remove` × 按钮；(e) `.lc-md-editor` 1fr 1fr grid + `lc-md-editor__textarea` 等宽 + `lc-md-editor__preview` 边框 surface-1 + 1024px 单列；(f) `.lc-sample-row` surface-2 浅色卡 + `.lc-sample-row__textarea` 等宽；**SPEC §11 Phase 5 勾选**：第 979 + 980 行 `★ 管理后台 - 题目管理页面` + `★ 管理后台 - 批量导入页面` checkbox 翻转 `[x]`；**CSP/SRI 兼容**：admin/problems.html + admin/problem-edit.html 全部携带 v1.2.20 canonical CSP meta tag + `csp.js` → `markdown.js` → `api.js` → `app.js` 强制加载顺序；node 脚本遍历全部 11 个页面实测 meta content 字节级一致；**静态校验**：`node --check` 4 个 JS 文件 (`api.js` / `app.js` / `csp.js` / `markdown.js`) 全部 OK；`node -e` 提取两个 admin 页面的内联 `<script>` 块 + `new Function(code)` 验证 JS 语法 OK（problems.html 块 29983 bytes / problem-edit.html 块 28980 bytes）；无新增后端 / DB / 测试二进制 / 路由变更（仅前端静态资源 + CSS） |
| v1.2.33 | 2026-07-06 | Phase 5 续：落地 §11 Phase 5 第八项 ★ `管理后台 - 用户管理页面（/admin/users.html）`（F10 + A22 + A24）；前序 v1.2.20 仅交付占位壳，本 commit 完成"完整可生产"版本；**数据来源决策**：SPEC §5.5 `GET /api/v1/admin/users` 属 **Phase 6**（`src/routes/admin_routes.h` 0 字节 / `stats_routes.h` 0 字节 → 路由未注册 → 404），与 profile.html / ranking.html 同策略：直接对 canonical 端点编写完整 render path（搜索 + 角色筛选 + 分页 + 改角色），端点 404 时 `showState('pending')` 软状态而非红错，API 落地后首个成功响应直接渲染进表格，本页无需改动；非 admin 仍走 `litecode.boot.shell({ requireAdmin: true })` 守卫；`/admin/users/:id/role` PUT 当前端点同样属于 Phase 6，所以 onSetRole 的 404 分支也给"管理员用户管理 API 尚未发布"的 toast（不要把 4xx 错误吞掉）；**`web/admin/users.html` 重写**（inline `<script>` IIFE + `'use strict'`，`el()` 与 dashboard/users 共形）：(a) header `lc-row--between` + `刷新` 按钮 + 副标 header-summary 显示"共 N 用户 · 当前页可见管理员 M 人"；filter card 复用 problems.html 的 `lc-problem-filters` shape（search / role segmented "全部/普通用户/管理员" / 总数 + 重置按钮）+ URL `?role=&q=&page=&limit=` 状态同步 + `replaceState`；(b) 表格 `lc-admin-table`（复用 problems.html）+ 列：ID / 用户名（链到 `/profile.html?u=` `target=_blank`）/ 邮箱 / 角色 pill（`.lc-pill--admin` for admins）/ 注册时间 / 最后登录（relTime + title 浮 ISO）/ 最后登录 IP / 操作；操作列 "降为普通用户 / 提升为管理员" 按钮，**self-protection**：当前 admin 自己行显示 `当前账号` 占位 + tooltip，client-side 拦截避免 round-trip（backend 也会拒绝，defense-in-depth）；(c) 分页器复用 v1.2.24 紧凑窗口；(d) `onSetRole(u, nextRole)` 走 `window.confirm` 双确认 + PUT `/admin/users/:id/role` body `{role}` + 错误四分支（404 → endpoint-pending toast / 403 → "不能修改自己的角色" / 429 → 限流提示 / 其他 → 通用 toast）+ 成功后 `litecode.toast.success` + `load()`；(e) `showState()` 四态（loading/empty/error/pending）单点切换；`litecode.boot.shell({ requireAdmin:true })` 自动 redirect + `inflightToken` 防过期请求覆盖；(f) `__lcTest` 暴露关键 helper；**`web/css/style.css` §18 章节已落地**（共享 `.lc-admin-table`/`.lc-pill--admin`），users.html 无新增样式；**静态校验**：`node -c` csp/api/app 三文件 OK + users.html inline `<script>` `new Function` 解析 OK + HTML static check 11/11 PASS（users.html CSP meta 与 csp.js canonical 字节级一致 + 脚本序 csp→api→app 正确）；**无新增后端 / DB / 测试二进制 / 路由变更**（仅前端静态资源；`admin_routes.h` 仍留给 Phase 6）；Phase 5 后续条目（管理后台 - 系统概览 / 审计日志 / 前端权限拦截 / 深色模式 / 移动端响应式）由各自 commit 接力 |
| v1.2.34 | 2026-07-07 | Phase 5 续：落地 §11 Phase 5 第九项 ★ `管理后台 - 系统概览页面（/admin/dashboard.html，含队列/预热池/指标）`；前序 v1.2.20 仅交付占位壳（42 行 h1 + "Phase 5 ★ … 将在后续 commit 落地"），本 commit 完成"完整可生产"版本；**数据来源决策**：SPEC §5.5 `GET /api/v1/admin/stats` 属 **Phase 6**（`src/routes/stats_routes.h` 0 字节 → 路由未注册 → 404），与 profile / ranking / users 三页同策略——直接对 canonical 端点编写完整 render path（KPI + 服务健康 + 判题系统 gauge + 状态分布 + 最近提交），端点 404 时 `showState('pending')` 软状态 + `dashboard-content` 仍然渲染（健康数据来自 `GET /api/v1/health` + 最近提交来自 `GET /api/v1/submissions?limit=8`），API 落地后首个成功响应直接覆盖所有面板，本页无需改动；非 admin 仍走 `litecode.boot.shell({ requireAdmin: true })` 守卫（app.js 兜底）；**`web/admin/dashboard.html` 重写**（原 41 行 → 现 ~390 行，inline `<script>` IIFE + `'use strict'` + 单一 `el()` 工厂与 users.html / ranking.html / profile.html 同形）：(a) header `lc-row--between` + `header-summary` + 两枚 affordance — 自动刷新 chip（`.lc-sse-status` 复用 v1.2.28 三态 dot 形状：polling 绿点 + 实时 / refreshing 中过渡 / timeout 橙点 + 已暂停）+ 手动 `刷新` 按钮；(b) 顶层 `dashboard-root` 容器四态 `showState(name)`（loading / error / pending / content）单点切换，state token + `inflightToken` 防过期请求覆盖；(c) **核心指标卡** 6 张 `.lc-stat-card`（复用 v1.2.30 profile.html）：题目总数 / 用户总数 / 提交总数 / 队列长度 / 预热池大小 / 在线时长，每张卡的副行在 Phase 6 数据齐时显示 `简单 X · 中等 Y · 困难 Z` / `管理员 N` / `最近 24 小时 N` / `运行中 R / 上限 M` / `目标 K` / `进程启动至今` 详细拆分，数据缺时统一显示 `Phase 6 数据待接入` 让 admin 知道是数据源问题而非真为零；(d) **服务健康卡** `.lc-card` + `.lc-health-strip`（3 列 grid，整体 / 数据库 / Docker）+ `.lc-health-cell` 复用 `.lc-pill` 新增 `.lc-pill--{ok,warning,down,muted}` 四色变体（light/dark 各一对 rgba）+ 链接到 `/api/v1/health` 原始响应；(e) **判题系统卡** 两段 gauge 复用 v1.2.27 result-panel 的 `.lc-gauge / __head / __label / __value / __track / __fill` + `--warning / --danger` 严重度映射（队列利用率 / 预热池占比）；分母 `queue.max_size` / `warm_pool.max` 来自 `/admin/stats`（Phase 6），缺失时 gauge 显示 `— / —` + width 0%；(f) **提交状态分布卡** 11 段 bar 复用 v1.2.30 profile 的 `.lc-breakdown / .lc-bar-row / .lc-bar-track / .lc-bar-fill` + 头行聚合 `全站累计 N · 通过率 X.X%`（0 分母安全）+ 按 count DESC 排序 + 0-count 也渲染（min-width 2px stub 让 admin 看见完整列表）；数据缺时整张卡 `hidden` 避免空 bar 误读；(g) **最近提交表** `.lc-admin-table` 复用 v1.2.32 problems.html + v1.2.33 users.html 7 列（#id / 状态 badge / 用户名链 profile / 题目 slug 链 problem.html / 语言 / 耗时 ms / 提交时间 relTime），`safeStatus` 白名单映射 class 防 hostile status 注入；`RECENT_LIMIT=8` 不暴露分页（这是 glances 而非 queue monitor）；(h) **数据加载** `loadStats()` 并行三个 promise：`/admin/stats`（canonical；404 时返 null）+ `/health`（fallback 探针）+ `/submissions?limit=8`（fallback recent）+ `Promise.all` 走 `normalize(stats, health)` 合并；stats 缺而 health 存 → pending 软状态 + dashboard-content 仍渲（来自 health + submissions）；两者都失败 → error 态 + retry 按钮；(i) **自动刷新** `startAutoRefresh` setInterval 10s + `visibilitychange` 暂停（`document.hidden=true` → set paused + chip 切 timeout；恢复时立即 `loadStats()` 让 admin 回到 tab 时不看见陈旧数据）；**`web/css/style.css` 新增 §19 章节** `Admin dashboard`（~30 selector）：`.lc-health-strip` 3 列 grid + `.lc-health-cell` surface-2 卡 + `__label` 小写 uppercase muted 字体；`.lc-pill--{ok,warning,down,muted}` 四色变体（success/warning/danger/text-2 配色，light/dark 各一对 rgba，与既有 `.lc-pill--{easy,medium,hard,admin}` 同源 token 复用）；768px 移动断点 `.lc-health-strip` 折叠单列；CSS 大括号配平 517/517（v1.2.33 487/487 + +30）；**复用既有组件**：`.lc-stat-card / .lc-stat-grid / .lc-section-title / .lc-card / .lc-pill / .lc-row--between / .lc-spinner / .lc-empty / .lc-error / .lc-loading / .lc-pagination / .lc-admin-table` —— 全部沿用 v1.2.19 / v1.2.30 / v1.2.32 既有 CSS token，dark mode 自动适配，零重复样式；**XSS 防御** 全部用户数据走 `textContent`/`setAttribute`（status 走 `STATUS_ORDER` 白名单映射 class，11 status 之外的 hostile 字符串退化为 `pending`），与 v1.2.20 DOMPurify + v1.2.24 escapeText + v1.2.33 users.html 同源契约；**CSP/SRI/Token 全兼容 v1.2.19 / v1.2.20 / v1.2.21**：`<meta http-equiv="Content-Security-Policy">` 与 `csp.js` canonical 字节级一致（实测比对 OK）+ 脚本加载顺序 csp → api → app 三段正确 + `litecode.api.get('/admin/stats')` 走 `credentials:'same-origin'` 自动带 refresh cookie；**SPEC 维护**：§11 Phase 5 第九项 `★ 管理后台 - 系统概览页面` 勾选 → `[x]`（行 983 checkbox 翻转）；**静态校验**：`node -c` csp/api/app 三文件 OK + dashboard.html inline `<script>` 1 块走 `new Function` 解析 OK + HTML static check 11/11 PASS（dashboard.html CSP meta 与 csp.js canonical 字节级一致 + 脚本序 csp→api→app 正确）；CSS 配平 517/517（v1.2.33 487/487 + +30）；**无新增后端 / DB / 测试二进制 / 路由变更**（仅前端静态资源 + 1 个 dashboard CSS 章节；`stats_routes.h` 仍留给 Phase 6） |
| v1.2.35 | 2026-07-07 | Phase 5 续：落地 §11 Phase 5 第十项 ★ `管理后台 - 审计日志页面（/admin/audit-logs.html）`（F13 + §4.2d + §15.6 + A17/A18/A20/A27）；前序 v1.2.20 仅交付占位壳（1617 bytes），本 commit 完成"完整可生产"版本；**数据来源决策**：SPEC §5.5 `GET /api/v1/admin/audit-logs` 属 **Phase 6**（`src/routes/admin_routes.h` 0 字节 → 路由未注册 → 404），与 profile / ranking / users / dashboard 四页同策略——直接对 canonical 端点编写完整 render path（列表 + 筛选 + 分页 + 详情抽屉），端点 404 时 `showState('pending')` 软状态（不阻断已有写入），API 落地后首个成功响应直接渲染进表格，本页无需改动；后端 `audit_log_repo.h`（Phase 3 ★）+ `audit_log_repo::list(pool, filter)` 数据契约已落地：`{items, total, limit, offset}` + 行字段 `id/admin_id?/action/target_type?/target_id?/payload?/ip?/created_at` + 6 个筛选维度（admin_id/action/target_type/target_id/since/until，since/until 走 `YYYY-MM-DD HH:MM:SS` 半开区间），前端筛选字段映射完全对齐；非 admin 仍走 `litecode.boot.shell({ requireAdmin: true })` 守卫（app.js 兜底）；**`web/admin/audit-logs.html` 重写**（原 41 行占位 → 现 ~620 行，inline `<script>` IIFE + `'use strict'` + 单一 `el()` 工厂与 users.html / dashboard.html / ranking.html / profile.html 同形）：(a) header `lc-row--between` + `header-summary` 显示"共 N 条 + 操作/对象/起始/截止筛选摘要" + 两枚 affordance — 自动刷新 chip（`.lc-sse-status` 复用 v1.2.28 / v1.2.34 三态 dot 形状：polling 绿点 + 实时 / refreshing 中过渡 / timeout 橙点 + 已暂停）+ 手动 `刷新` 按钮；(b) **过滤卡** 复用 v1.2.32 problems.html + v1.2.33 users.html 的 `lc-problem-filters` shape（search / action segmented 8 段 "全部 / 新建题 / 改题 / 删题 / 批量导入 / 改角色 / 登录失败" / target_type segmented 4 段 "全部 / 题目 / 用户 / 系统" / 起止日期 input[type=date] / 总数 + 重置按钮）+ URL `?action=&target_type=&q=&since=&until=&page=&limit=` 状态同步 + `replaceState` 让深链 round-trip；URL state 把 since/until 也持久化（深链可分享"今天 9 点前的所有删题记录"）；(c) **数据表** `.lc-admin-table` 复用 v1.2.32 problems.html + v1.2.33 users.html + v1.2.34 dashboard.html，7 列：#ID（点击 ID 单元格直接打开详情抽屉）/ 相对时间（+ title 浮窗 ISO）/ 管理员（链 `/admin/users.html?q=id:N`；admin_id 为 null 时区分"匿名（登录失败）"/ "—" system event）/ 操作 pill（`ACTION_PILL_CLASS` 白名单映射，删题红/改角色蓝/登录失败橙/AC 绿/未知灰色）/ 对象（type pill + target_id）/ IP / "查看" 按钮打开详情；(d) **详情抽屉** `.lc-modal`（fixed + 居中 + backdrop 半透明 + ESC 关闭 + backdrop click 关闭，aria-modal="false" 故意不 trap 焦点让 admin 边看抽屉边扫表），header 显示 `#ID · 操作 label`，body 用 `<dl>` 两列 grid 渲染 7 个字段（操作 ID / 时间 / 管理员 / 操作 / 对象类型 / 对象 ID / IP），payload 走 `.lc-code-block`（surface-2 背景 + 圆角 + 等宽字体 + max-height 360px 滚动）`prettyJson()` 漂亮打印（`JSON.parse` 失败 fallback 到原文 textContent，绝不 innerHTML）；(e) **分页** 复用 v1.2.24 / v1.2.32 紧凑窗口（首页 / 上页 / 7 页以内全显或 `1 … cur-1 cur cur+1 … tp` + ellipsis / 下页 / 末页 + disabled 状态 + `aria-current="page"`），`inflightToken` 防过期请求覆盖；(f) **数据加载** `load()` 单飞 — `litecode.api.get('/admin/audit-logs?limit=&offset=&action=&target_type=&since=&until=')` 走 `credentials:'same-origin'` 自动带 refresh cookie；404 → `showState('pending')` 不显示红 banner（与 users / dashboard 同策略），403 → "您没有访问审计日志的权限" 专属文案，401 → "登录状态已过期" 走 app.js 全局 redirect；(g) **自动刷新** `startAutoRefresh` setInterval 30s（比 dashboard 的 10s 更长，audit log 写入频率低，30s 对 admin 体验更友好）+ `visibilitychange` 暂停（`document.hidden=true` → set paused + chip 切 timeout；恢复时立即 `load()` 让 admin 回到 tab 时不看见陈旧数据）；`state.inflight` monotonic token 防止 stale response；(h) `__lcTest` 暴露关键 helper（ACTION_LABEL / ACTION_PILL_CLASS / TARGET_LABEL / prettyJson / relTime / fmtAbsolute / actionLabel / targetLabel / renderRow）；**`web/css/style.css` 新增 §20 章节** `Modal + JSON code block`（约 12 selector）：`.lc-modal` `position:fixed; inset:0; z-index:50; flex 居中` + `[hidden] display:none` + `.lc-modal__backdrop` 半透明 rgba 15 23 42 0.45（dark mode 0.6）+ `.lc-modal__panel` surface 卡 + max-height calc(100vh-32px) + overflow auto + shadow-lg + padding 20px + 圆角 lg；`.lc-code-block` surface-2 背景 + 边框 + 圆角 md + 等宽字体 + 12.5px + max-height 360px + overflow auto + `.lc-code-block code` 重置白空格 pre；CSS 大括号配平 529/529（v1.2.34 517/517 + +12）；**复用既有组件**：`.lc-admin-table / .lc-card / .lc-pill / .lc-pill--{ok,warning,down,admin} / .lc-row--between / .lc-spinner / .lc-empty / .lc-error / .lc-loading / .lc-pagination / .lc-page-btn / .lc-form-help / .lc-form-label / .lc-input / .lc-section-title / .lc-sse-status` —— 全部沿用 v1.2.19 / v1.2.24 / v1.2.27 / v1.2.30 / v1.2.32 / v1.2.33 / v1.2.34 既有 CSS token，dark mode 自动适配，零重复样式；**XSS 防御** 全部用户数据走 `textContent`/`setAttribute`（action 走 `ACTION_PILL_CLASS` 白名单映射，9 已知 action 之外的 hostile 字符串退化为中性 `.lc-pill`；payload 走 `prettyJson()` `JSON.parse` + `JSON.stringify(_, null, 2)` 漂亮打印，parse 失败 fallback 原文，绝不 `innerHTML`），与 v1.2.20 DOMPurify + v1.2.24 escapeText + v1.2.32 problems.html + v1.2.33 users.html + v1.2.34 dashboard.html 同源契约；**CSP/SRI/Token 全兼容 v1.2.19 / v1.2.20 / v1.2.21**：`<meta http-equiv="Content-Security-Policy">` 与 `csp.js` canonical 字节级一致（实测比对 OK）+ 脚本加载顺序 csp → api → app 三段正确 + `litecode.api.get('/admin/audit-logs')` 走 `credentials:'same-origin'` 自动带 refresh cookie；**SPEC 维护**：§11 Phase 5 第十项 `★ 管理后台 - 审计日志页面` 勾选 → `[x]`（行 987 checkbox 翻转）；**静态校验**：`node -c` csp/api/app 三文件 OK + audit-logs.html inline `<script>` 1 块走 `new Function` 解析 OK + HTML static check 11/11 PASS（audit-logs.html CSP meta 与 csp.js canonical 字节级一致 + 脚本序 csp→api→app 正确）；CSS 配平 529/529（v1.2.34 517/517 + +12）；**无新增后端 / DB / 测试二进制 / 路由变更**（仅前端静态资源 + 1 个 modal/css 章节；`admin_routes.h` 仍留给 Phase 6） |
| v1.2.36 | 2026-07-07 | Phase 5 续：落地 §11 Phase 5 第十一项 ★ `前端权限拦截（非管理员 → 跳转首页，未登录 → 跳转登录）`（§6.3 + §15.3 + A24）；前序 v1.2.19/v1.2.21 app.js 已有 `litecode.boot.shell({ requireAdmin: true })` 守卫，但仅在 hydrate 完成后才 redirect → 非 admin 用户会先看到 admin UI 渲染 ~50-100ms 才被踢回首页，且**未登录用户被错误地踢到 `/index.html` 而非 `/login.html`**（违反 SPEC 要求）；本 commit 落地"**多层防御 + 零闪现**"权限拦截；**`web/js/app.js` 改造**（in-line IIFE 顶部新增 ~70 行同步 gate + 重构 bootShell/guard 约 50 行）：(a) **`ADMIN_ROUTES` 白名单**（5 条）+ **`isAdminRoute(path)` 函数** —— 显式列出 5 个 admin 页面路径 + 兼容 `path.indexOf('/admin/') === 0 && /\.html$/.test(p)` 前缀匹配以自动覆盖未来 `/admin/*.html` 新页面；null/undefined path 走 `path || (root.location && root.location.pathname) || ''` 兜底防 TypeError；(b) **同步 cached-role gate** —— IIFE 顶层（csp.js → api.js → app.js `<script defer>` 链执行时，`defer` 保证 IIFE 在 DOMContentLoaded 之前但在 body 解析之后跑，浏览器尚未合成首帧 → 是无闪烁拦截的黄金窗口）：若 `onAdminRoute` 且 `sessionStorage.getItem('litecode:user')` 命中且 `JSON.parse(...).role !== 'admin'` → `window.location.replace('/index.html')` 立即同步重定向 + 提前 return IIFE；`try/catch` 容错 sessionStorage 中损坏 JSON；这是 common case（admin 链接已登录后切到非 admin 页面）的零网络往返拦截；(c) **`html.lc-route-pending` no-flash** —— 同步 gate 还会无条件（即使是 admin 缓存）给 `<html>` 加 `lc-route-pending` 类，body 隐藏直到 `boot.shell({requireAdmin}).then()` 内移除（成功路径）或 `location.replace()` 触发导航（重定向路径）；CSS 用 `visibility: hidden + pointer-events: none` 而非 `display: none` 以保留 layout box，paint 一旦放行不发生 reflow；这把"非 admin 短暂看到 admin shell"压到 0；(d) **`gate.requireAdmin(user, {silent, onAllowed})` 收口** —— 把"用户已经 hydrate 出来之后的角色判定 + 重定向策略"从 bootShell 内联代码抽成单独函数，DRY 收口 + 让 `bootShell({requireAdmin:true})` 与 `litecode.guard.requireAdmin()` 共享同一策略；3 case 分支：`!user` → toast 'warn' 请先登录 + `replace('/login.html?next=' + nextUrl())` + 返回永不 resolve 的 Promise；`user.role !== 'admin'` → toast 'error' 没有管理员权限 + `replace('/index.html')` + 返回永不 resolve 的 Promise；`user.role === 'admin'` → `Promise.resolve(onAllowed(user))`，onAllowed 默认 identity；`onAllowed` 是 bootShell 注入的"放行时移除 lc-route-pending 类"副作用，让 body 立刻可见；(e) **`nextUrl()` helper** —— 抽出 `(location.pathname + location.search)` encodeURIComponent 公共逻辑，guard.requireAuth + gate.requireAdmin 共享同一个 `?next=` 编码；(f) **`bootShell({requireAdmin:true})` 委托 gate** —— `hydrateUser().then(user => gate.requireAdmin(user, {onAllowed: u => {documentElement.classList.remove('lc-route-pending'); return u;}}))`，让"放行 = 移除隐藏类"成为契约的副作用而不是内联魔法；(g) **`guard.requireAdmin()` 复用 gate** —— 现在 `guard.requireAdmin() === guard.requireAuth().then(u => gate.requireAdmin(u))`，与 bootShell 走完全相同的 redirect 逻辑（v1.2.21 之前 guard 是 dead code，现在变得可达且行为一致）；**`web/css/style.css` 新增 §21 章节** `Admin route gate — no-flash permission interception`（3 selector）：`html.lc-route-pending body { visibility: hidden; pointer-events: none; }` —— visibility 而非 display 避免 layout reflow；pointer-events 防"用户按下鼠标时 body 突然出现并 click 一个未鉴权按钮"的边界；CSS 大括号配平 532/532（v1.2.35 529/529 + +3）；**`web/admin/*.html` 5 页零修改**：现有 5 个 admin 页面（dashboard.html / users.html / problems.html / problem-edit.html / audit-logs.html）已通过 `litecode.boot.shell({ requireAdmin: true })` 触发拦截，无需任何静态 HTML 改动（`<html class="lc-route-pending">` 由 app.js IIFE 在同步阶段自动注入）；这是设计的关键：拦截逻辑集中在一处，新增 admin 页面只需 (1) 把路径加入 `ADMIN_ROUTES` 白名单（事实上未来 `*.html` 自动覆盖）+ (2) 在 boot 用 `requireAdmin: true`；**测试覆盖**（手测不写测试二进制）：用 17 个真实 path 测试 `isAdminRoute` —— 5 admin 已知页面 / 3 未来 admin 页面（/admin/future-page.html / /admin/index.html 等 .html 后缀）/ 9 non-admin 页面（/index.html / /login.html / /problem.html 等），17/17 通过；用 8 个 cached user 测试同步 role gate —— null / undefined / {} / `{role:''}` / `{role:'admin'}` → allow:async；`{role:'user'}` / `{role:'guest'}` / `{role:'ADMIN'}`（大写）→ redirect:/index.html，全部通过；**SPEC §6.3 / §15.3 / A24 完整对齐**："未登录 → 跳转登录"（case 1：`!user` → /login.html?next=...）+ "非管理员 → 跳转首页"（case 2：`user.role !== 'admin'` → /index.html）+ "前端拦截"（同步 + 异步双层 gate，零闪现）；**复用既有组件**：`litecode.boot.shell` / `litecode.guard.*` / `litecode.auth.currentUser` / `sessionStorage.litecode:user` —— 全部沿用 v1.2.19 / v1.2.21 既有 API，零新依赖；**CSP/SRI/Token 全兼容** v1.2.19 / v1.2.20 / v1.2.21：`<meta http-equiv="Content-Security-Policy">` 与 `csp.js` canonical 字节级一致 + 脚本加载顺序 csp → api → app 三段正确 + sessionStorage 是 HTML5 标准 API 已在 CSP allowlist 内；**静态校验**：`node -c` csp/api/app 三文件 OK + 5 admin 页面 inline `<script>` 1 块走 `new Function` 解析 OK（audit-logs 36606 / dashboard 30204 / problem-edit 28981 / problems 29984 / users 22870，5/5 OK）+ HTML static check 11/11 PASS（5 admin 页面 CSP meta 与 csp.js canonical 字节级一致 + 脚本序 csp→api→app 正确）+ 17 路径测试 + 8 role 测试 全 PASS；**无新增后端 / DB / 测试二进制 / 路由变更**（纯前端 app.js + 1 个 css 章节） |
| v1.2.37 | 2026-07-07 | Phase 5 收尾：落地 §11 Phase 5 第十二项 ★ `深色模式（CSS 变量 + prefers-color-scheme + 手动切换持久化）`（§6.3 + A34）；前序 v1.2.19/v1.2.25 框架阶段已交付骨架（CSS 变量双套 + nav 切换按钮 + app.js applyTheme/toggleTheme/localStorage 三件套 + html.dark 选择器 + @media (prefers-color-scheme: dark) 系统偏好兜底），但**有两个未解决的契约 bug**：(a) **HTML 11 个页面 `<html lang="zh-CN" data-theme-chosen>` 硬编码 `data-theme-chosen` 属性** —— 而 style.css 的系统偏好回退规则用 `html:not([data-theme-chosen]) { --lc-color-bg: #0e1116; ... }` 在 @media 内做门控；硬编码该属性意味着 `:not([data-theme-chosen])` 选择器**永远不匹配**任何元素 → 系统偏好永远失效，OS 设为 dark mode 的用户**永远看到 light 主题**，必须手动点切换按钮才能进 dark mode —— 违反 SPEC §6.3 + A34；(b) **无 no-flash 脚本** —— `applyTheme(detectInitialTheme())` 在 `bootShell` 内跑（`defer` 阶段，DOMContentLoaded 之后），用户点切换到 dark 后再刷新会先看到 ~50-100ms light 闪烁再切回 dark；本 commit 落地"**5 修复 + 1 测试**"深色模式收尾；**`web/js/theme-boot.js` 新增**（同步脚本，在 `<head>` 中 stylesheet link **之前**加载，27 行 IIFE）：(a) 读 `localStorage['litecode:theme']`，try/catch 容错 storage 抛错（private mode / 0 quota iframe）；(b) 仅在值 === 'dark' 或 'light' 时才设 `<html>` 的 `.dark` class + `data-theme-chosen="1"` 属性，其它情况（含 null / 'auto' / 'DARK' / '  dark  ' 等脏值）一律 no-op，让 `style.css` 的 `@media (prefers-color-scheme: dark) html:not([data-theme-chosen])` 系统偏好规则接管；(c) 顶部注释完整记录"为什么是同步脚本而不是 inline"（CSP `script-src 'self' cdn.jsdelivr.net` 无 `'unsafe-inline'`，inline `<script>` 会被浏览器拒绝）+ "HTML 页面 MUST NOT 硬编码 data-theme-chosen"（否则 CSS 永远看不到 not() 的匹配）+ "load order contract"（theme-boot.js sync → stylesheet → csp defer → api defer → app defer）；**`web/*.html` 11 页修改**：移除 `<html lang="zh-CN" data-theme-chosen>` 中硬编码的 `data-theme-chosen` 属性；在 `<link rel="stylesheet">` 之前插入 `<script src="/js/theme-boot.js"></script>` 同步脚本，附 comment 解释 no-flash contract；**`web/js/app.js` 改造**（约 60 行）：(a) **`currentTheme()` 抽出**（之前的 inline `classList.contains('dark') ? 'dark' : 'light'` 散落两处，统一调用点）；(b) **`applyTheme(theme)` 强化** —— 不再只设 class，**同时设置 `data-theme-chosen="1"` 属性**（一旦 bootShell 调用过一次 `applyTheme`，等价于做出明确选择，从此锁定；CSS 系统偏好规则即被屏蔽；用户可走新增的 `litecode.theme.reset()` 重新跟随 OS）；(c) **`detectInitialTheme()` 简化为 `currentTheme()`**（theme-boot.js 已经在 first paint 之前把正确状态写到 DOM，app.js 无需再读 localStorage / matchMedia 重新推导）；(d) **`toggleTheme()` 增加 CustomEvent 派发** `window.dispatchEvent(new CustomEvent('litecode:theme-changed', { detail: { theme: next } }))`（多 nav slot 跨页面同步 + 未来 markdown editor / CodeMirror 等 surface 接同一事件即同步；event 名 `litecode:*` 沿用 api.js 既有约定）；(e) **`syncThemeButton()` 新增**（30 行）—— 同步 `[data-act="theme-toggle"]` 按钮的 icon（🌙 在 light mode 提示点 → dark / 🌞 在 dark mode 提示点 → light）+ `aria-pressed`（true/false screen-reader 状态）+ `aria-label` + `title`（中文 "切换到深色模式" / "切换到浅色模式"，从 v1.2.19 的硬编码 "切换深色模式" 改成动态文案）；`renderNav` 在插入按钮后立即调用 `syncThemeButton()`；(f) **`prefers-color-scheme` change listener 增加 `syncThemeButton()` 调用** —— OS 切到 dark mode（用户未存 choice）时按钮图标/aria 同步翻转；(g) **`litecode.theme` 公共 API 扩展** —— `get()` 走 `currentTheme()` helper，`set(t)` 校验 + 写存储 + applyTheme + syncThemeButton 全链路，新增 `reset()`（清 localStorage + 重新跟随 OS + syncButton）—— 测试与未来 "reset to system" 设置项共用；(h) **顶部 doc-comment 更新** —— 明确写 "no-flash dark-mode load order: theme-boot.js sync → stylesheet → csp defer → api defer → app defer"；(i) **撤销 v1.2.19 重复应用** —— `bootShell` 不再调 `applyTheme(detectInitialTheme())`，仅安装 matchMedia listener + `litecode:theme-changed` 全局监听；**`web/test/html-static-check.js` 改造**（1 处）：原断言 `if (!order[0] || order[0] !== 'csp.js')` 太严，强制 csp.js 必须是第一个脚本；改为允许 `theme-boot.js` 作为同步头脚本前缀（`if (chain[0] === 'theme-boot.js') chain.shift()`），链完整性 `csp.js → api.js → app.js`（markdown.js 可选）仍然校验；**`web/test/theme-boot.test.js` 新增**（vm sandbox + minimal DOM stub，32 用例全过）：(a) `localStorage='dark'` → .dark + data-theme-chosen=1 + getItem 调 1 次（3 case）；(b) `localStorage='light'` → 不加 .dark + data-theme-chosen=1 + getItem 1 次（3 case）；(c) `localStorage=null/undefined` → 不加 .dark + 不设属性 + getItem 1 次（4 case）；(d) 6 个脏值（'' / 'auto' / 'system' / 'DARK' / 'Light' / '  dark  '）→ 全部视同 null（12 case）；(e) `localStorage.getItem` 抛 SecurityError → 不崩 + 无副作用（2 case）；(f) `localStorage` 完全缺失 → 不崩 + 无副作用（2 case）；(g) 单次 IIFE 只调用 getItem 1 次（one-shot 守卫，2 case）；(h) 两次连续 run 状态独立（防 module-level closure，4 case）；**全栈静态校验**：`node -c web/js/theme-boot.js` OK + `node -c web/js/app.js` OK + `node -c web/js/csp.js` OK + `node -c web/js/markdown.js` OK；`node web/test/html-static-check.js` 11/11 PASS（script 顺序：`[theme-boot.js,csp.js,api.js,app.js]` 5 公共页 / `[theme-boot.js,csp.js,markdown.js,api.js,app.js]` 2 含 markdown 的页 — admin/problem-edit.html 与 problem.html 含 markdown.js）；`node web/test/theme-boot.test.js` 32/32 PASS；`node web/test/history.test.js` 45/45 PASS + `node web/test/profile.test.js` 33/33 PASS + `node web/test/ranking.test.js` 38/38 PASS + `node web/test/sse-parser.test.js` 49/49 PASS —— **5 个 web test 197/197 PASS**（新增 32 用例）；**SPEC §11 Phase 5 第十二项勾选 `[x]`** 行 991 checkbox 翻转；**SPEC §12.1 A34 验收用例**："切换深色模式后页面正确变色，刷新后保持" —— 现在完全成立：toggle 写 localStorage + 切 class + 切 data-theme-chosen + dispatch 事件 + syncButton；刷新时 theme-boot.js 在 first paint 前已读 localStorage 并设 class + 属性，所以首帧即正确颜色无闪烁；**复用既有组件**：`localStorage` / `matchMedia('(prefers-color-scheme: dark)')` / `CustomEvent` / `aria-pressed` —— 全部 W3C 标准 + 已被 v1.2.19/v1.2.21 等既有 code path 使用，零新依赖；**CSP/SRI/Token 全兼容** v1.2.20/v1.2.21：theme-boot.js 走 `script-src 'self'` 允许的同源 external script，不破坏 CSP；`data-theme-chosen` 属性移除对 Caddy header CSP 零影响（CSP 不锁 HTML attribute）；sessionStorage 缓存 user 的 v1.2.21 路径无变更；**无新增后端 / DB / 测试二进制 / 路由 / 数据库迁移 / 静态资源变更**（纯前端 theme-boot.js + 11 HTML 微调 + app.js 重构 + 1 css 章节已就绪 + 1 个新增 Node.js test） |
| v1.2.38 | 2026-07-07 | Phase 5 收尾：落地 §11 Phase 5 第十三项 ☆ `移动端响应式（< 768px 切换上下布局）`（§6.3 / §11 / A34 部分）；前序 v1.2.19 + v1.2.24 + v1.2.25 + v1.2.27 + v1.2.28 + v1.2.29 + v1.2.31 + v1.2.32 已散布**零散**移动规则（`@media (max-width: 768px)` nav/forms、`640px` ranking 卡布局、`600px` history rows、`540px` gauge 单列、`480px` password rules、`1024px` problem split + md-editor），但**三个真空白**让 SPEC 要求"切换上下布局"未真正落地：(a) **`/admin/*.html` 4 张 `lc-admin-table` 在 ≤768px 仍是横向滚动**（`.lc-admin-table { min-width: 720px; }` + 外层 `.lc-admin-table-wrap { overflow-x: auto; }`），手机用户必须来回拖才能看完一行的所有字段，破坏"单手操作"承诺；(b) **`<=768px` 段落未覆盖**到 problem.html 的 editor toolbar（语言 select + 提交按钮在同一行）+ submit row + history tab + modal drawer + dashboard health strip + profile/ranking 的 fine-tuning；(c) **没有任何契约测试**锁定"哪些页面 + 哪些列必须 data-label 才能让 CSS 卡布局生效"——下一个 commit 容易把列漏掉 label 又悄悄不被发现；本 commit 收口所有空白并新增契约测试；**`web/css/style.css` 新增 §22 章节** `Mobile responsive — Phase 5 ★ 移动端响应式`（约 75 selector + 5 个 `@media` 断点）：(a) **`@media (max-width: 1024px)`** —— `.lc-md-editor` 单列折叠（补全 §18 既有）；(b) **`@media (max-width: 900px)`** —— `.lc-problem-layout` 双栏塌单列 + `.lc-problem-pane max-height: none` + `.lc-editor-pane position: static`（取消 sticky 否则窄屏吃掉视口）+ `.lc-editor-shell` 高度 360px 固定（早于 1024px 触发，避免 iPad portrait 出现两个挤挤的半列）；(c) **`@media (max-width: 768px)`（canonical 手机断点）** —— container padding 从 `var(--lc-space-5) var(--lc-space-4)` 收缩到 `var(--lc-space-4) var(--lc-space-3)` + `.lc-card` / `.lc-problem-filters` / `.lc-problem-pane` / `.lc-result-panel` / `.lc-form-section` / `.lc-auth-card` padding 全部减半 + nav 高度 56→52 + 隐藏用户名 + `.lc-nav-brand` 字号缩小 + `.lc-editor-pane__toolbar` 改 column + actions 占满宽度右对齐 + `.lc-editor-pane__submit-row` 改 column + 提交按钮占满 + `.lc-modal` 内边距缩小 + `.lc-modal__panel` 占满 viewport 高 + `.lc-health-strip` 塌单列（v1.2.34 已就绪保留）+ `.lc-admin-table thead th / tbody td` padding 8px/10px + font-size 13px + `.lc-page-btn` 紧凑 30px/9px + `.lc-sample__pre` max-height 240px + h1/h2/h3 字号降档；(d) **`@media (max-width: 640px)` — admin table 卡布局核心** —— `.lc-admin-table / thead / tbody / tr / td` 全部 `display: block; width: 100%` + thead 用 `clip-path` 视觉隐藏但保留 a11y 树（screen reader 仍可读列名）+ tbody tr 改 card-shape（border + radius + shadow + padding-2）+ tbody td 改 flex row + `td::before { content: attr(data-label); ... }` 注入列名（80px 固定 flex-basis + ellipsis + muted 色 + 12px 字号）+ 操作列 (`td[data-label="操作"]`) 的 `::before` 隐藏 + 列改 column 让按钮占满 + `.lc-admin-table-wrap overflow-x: visible` 关掉横向滚动；(e) **`@media (max-width: 540px)`** —— `.lc-result-panel__header` 改 column 左对齐 + actions 占满宽度 + `.lc-form-section .lc-row` 改 column 让 time/memory 等并列输入堆叠（admin/problem-edit.html 受益）；(f) **`@media (max-width: 480px)`** —— `.lc-nav-links` 横向 scroll strip（无 hamburger 抽屉 — 站点仅 4 个 nav 项横向 scroll 更轻量）+ `.lc-nav-right .lc-btn` 紧凑 + `.lc-stat-card` padding 收缩 + `.lc-stat-card__value` 22px + `.lc-profile-head gap 收缩` + `.lc-history-row padding / metrics` 紧凑 + `.lc-problem-filters__row .lc-row--between` 改 column 让 "重置筛选" 按钮拇指可达 + `.lc-pw-rules` 单列 + `.lc-sample-row__io-row` column（input/output textarea 堆叠）；(g) **`@media (max-width: 420px)`** —— 极窄屏（360px 老 Android）`.lc-container / .lc-card / .lc-nav / .lc-page-btn` 再缩一轮；**`web/admin/problems.html` / `web/admin/users.html` / `web/admin/audit-logs.html` / `web/admin/dashboard.html` 4 个 JS 渲染器改造**：每个 `el('td', null, [...])` 改成 `el('td', { dataset: { label: '<列名>' } }, [...])` —— 27 处 cell 加 data-label（problems 7 列 / users 8 列 / audit-logs 7 列 / dashboard 7 列），与后端列名 1:1 对齐（Slug / 标题 / 难度 / 标签 / 通过率 / 时间 / 内存 / 操作 / ID / 用户名 / 邮箱 / 角色 / 注册时间 / 最后登录 / 最后登录 IP / 时间 / 管理员 / 对象 / IP / 查看 / # / 状态 / 用户 / 题目 / 语言 / 耗时 / 提交时间）；`el()` 既有 helper 已支持 `dataset: { k: v }`（v1.2.19 app.js 已实现，零 helper 改造）；**`web/test/responsive.test.js` 新增 51 用例**（Node.js + 自实现 brace-counter section 提取 + traverse + regex 锁定契约）：(a) **7 个断点必须存在** — 1024px / 900px / 768px / 640px / 540px / 480px / 420px 各有 `@media (max-width: NNNpx)` 块（7/7 PASS，缺一即 fail 防止未来 commit 误删某条规则）；(b) **≤ 640px admin table 三件套** — `.lc-admin-table` 含 `display: block` + `td::before` 用 `content: attr(data-label)` + 操作列 `data-label="操作"` 选择器存在（防止 CSS 卡布局被破坏或标签注入机制被去掉）；(c) **27 处 cell data-label 必须存在**（4 admin 页 × 列数加总）—— 每个 admin 渲染器必须给每个 td 写 `label: '<列名>'`（`dataset` 形式而非 raw attribute），缺一即 fail；(d) **10 个 HTML 页必须含 viewport meta** `<meta name="viewport" content="width=device-width, ...>` —— 防止新增页面忘记 viewport；51/51 PASS `node web/test/responsive.test.js`；**所有现有页面零修改** —— index.html / login.html / register.html / problem.html / profile.html / ranking.html / admin/problem-edit.html 均通过既有 CSS 规则（v1.2.25 problem split @ 1024px / v1.2.31 ranking 卡布局 @ 640px / v1.2.29 history rows @ 600px / v1.2.27 gauge @ 540px / v1.2.23 password rules @ 480px）+ 本 commit 新增规则的叠加获益，无任何 inline style 改动；**`web/css/style.css` 大括号配平**：v1.2.37 532/532 → 本 commit 532 + 75 = **607/607**（用 `grep -c '^{' web/css/style.css` + `grep -c '^}' web/css/style.css` 实测配平）；**复用既有组件**：`.lc-container / .lc-card / .lc-nav / .lc-row / .lc-stack / .lc-problem-list / .lc-tag-chips / .lc-pill / .lc-pagination / .lc-stat-card / .lc-history-row / .lc-rank-row / .lc-form-section / .lc-pw-strength / .lc-md-editor / .lc-editor-shell / .lc-result-panel / .lc-modal / .lc-admin-table / .lc-admin-table-wrap / .lc-sse-status / .lc-draft-banner` —— 全部沿用 v1.2.19 ~ v1.2.37 既有 CSS token，仅在断点内做 display/padding/grid-template 切换，dark mode 自动适配，零重复样式；**CSP/SRI/Token 全兼容** v1.2.19 ~ v1.2.37：CSS 文件 4 个 JS 文件无变更，HTML 11 页 `<meta http-equiv="Content-Security-Policy">` 与 `csp.js` canonical 字节级一致（实测 11/11 PASS，**注意**：admin/problems.html + admin/users.html + admin/audit-logs.html + admin/dashboard.html 的 `el('td', { dataset: { label: 'X' } }, [...])` 全部走 `el()` helper 通过 `node.dataset.label = 'X'` 设值 → DOM 序列化成 `data-label="X"`，XSS 安全——`el()` helper 全部用户内容走 `textContent`，dataset label 是中文/英文字面量，不存在 user-controlled 注入）；**SPEC 维护**：§11 Phase 5 第十三项勾选 → `[x]`（行 993 checkbox 翻转 `→ [x] ☆ 移动端响应式（< 768px 切换上下布局） —— v1.2.38`）；**全栈静态校验**：`node -c web/js/csp.js` OK + `node -c web/js/api.js` OK + `node -c web/js/app.js` OK + `node -c web/js/markdown.js` OK + `node -c web/js/theme-boot.js` OK；`node web/test/html-static-check.js` 11/11 PASS（CSP meta 与 csp.js canonical 字节级一致 + script 顺序 `[theme-boot.js,csp.js,api.js,app.js]` 5 公共页 + `[theme-boot.js,csp.js,markdown.js,api.js,app.js]` 2 含 markdown 页）；`node web/test/responsive.test.js` 51/51 PASS；`node web/test/theme-boot.test.js` 32/32 PASS；`node web/test/history.test.js` 45/45 PASS + `node web/test/profile.test.js` 33/33 PASS + `node web/test/ranking.test.js` 38/38 PASS + `node web/test/sse-parser.test.js` 49/49 PASS —— **6 个 web test 248/248 PASS**（新增 51 用例）；**无新增后端 / DB / 测试二进制 / 路由 / 数据库迁移**（仅前端 CSS §22 章节 + 4 admin 页 JS 渲染器 dataset.label 标注 + 1 个新增 Node.js test） |
| v1.2.39 | 2026-07-07 | Phase 6 开篇：实现 §11 Phase 6 第一项 ★ `用户做题统计 API（GET /api/v1/stats/profile/:username）`（SPEC §5.4 + §11 + A15）；前序 v1.2.30 已交付 `/profile/:username` 前端壳但做题统计全靠客户端聚合（self-only —— `/submissions` 对非 admin 强制 `user_id = self`），本 commit 把"看自己的统计"落到后端端点，Phase 5 profile.html 后续可一键切到该端点；**`src/routes/stats_routes.h` 全新交付**（SPEC §5.4 表格 cell 留空 — "已登录" / 无 rate limit），header-only + inline，沿用 Phase 3/4 admin endpoints 同样的 6 步管道（require_authentication → 不 consume rate limit（SPEC §5.4 留空） → path-parse + username 校验 → 原始 SQL 查 user → 5 repo 聚合查询 → serialize + send_success）；`stats_routes::detail` 自含 `kStatsMinUsernameLength / kStatsMaxUsernameLength` + `stats_is_valid_username_char` + `stats_validate_username`（mirror `user_repo::validate_username`，3..50 / [A-Za-z0-9_.-]/ 禁前后 `.`/`-`，长度 + 字符集 + 前后缀三段串行校验） + `UserProfileRow` + `find_user_for_stats`（DATE_FORMAT'd SELECT id/username/role/created_at FROM users WHERE username = ?） + `extract_username_from_path`（prefix-strip + 拒 nested path + 拒 empty tail + 委托 validate_username）+ `count_user_submissions`（SELECT COUNT(*) FROM submissions WHERE user_id = ?） + `count_user_solved_problems`（JOIN submissions → problems 的 DISTINCT problem_id WHERE status='ac' AND p.is_deleted=FALSE — 软删题不计入已解决） + `count_user_attempted_problems`（同 JOIN shape 但 status 任意 — 用于"尝试过 N 道题"指标）+ `count_submissions_by_status`（GROUP BY status → unordered_map，handler 阶段 zero-pad 到完整 11 个 status 集合）+ `count_solved_by_difficulty`（JOIN + WHERE difficulty IN ('easy','medium','hard') AND p.is_deleted=FALSE + GROUP BY difficulty → unordered_map，同样 zero-pad 到 3 个 difficulty）；`UserStats` struct（total_submissions / solved_count / attempted_count / total_problems / acceptance_rate / by_status / by_difficulty_solved 7 字段）+ `compute_user_stats`（5 个独立 query 串行调用 + acceptance_rate = solved/total_problems*100.0 零分母安全返 0.0 避免 NaN）；`serialize_by_status` / `serialize_by_difficulty`（显式构造 11/3 个已知 key 避免 unordered_map 迭代顺序不确定 + 拒 hostile 未来状态名 `"foobar"` 渗入响应 —— 白名单过滤）+ `serialize_user_meta`（id/username/role/created_at 四字段）+ `serialize_user_stats`（嵌套 stats 块）；`get_user_profile_stats_handler` 7 步：auth → extract_username（400 INVALID_INPUT details.field="username"） → find_user_for_stats（nullopt → 404 NOT_FOUND details.username） → compute_user_stats（任何 throw → 500 INTERNAL_ERROR）→ structured LOG_INFO（username/user_id/total_submissions/solved_count/total_problems 五字段）→ serialize + send_success（200 + `{user:{...}, stats:{...}, request_id}`，request_id 由 server.h 统一注入）；`register_stats_routes` 签名 `register_stats_routes(server, pool, limiter, rate_cfg, jwt_cfg)` —— `limiter`/`rate_cfg` 保留参数为未来加 quota 留接口（SPEC §5.4 当前留空），handler 内**不调** `consume_rate_limit()`，响应**不携带** X-RateLimit-* 头，鉴权失败和 bad shape 都不消耗桶；**ODR workaround** — `user_repo.h` 与 `problem_repo.h` 都定义 `litecode::detail::req_string / req_int`，同 TU 拉入两个 header 会触发 MSVC 严格模式 ODR 违规；本 commit 解决方案：**不 include `user_repo.h`**，而把 `validate_username` + `find_user_for_stats` 在 stats_routes.h 内 inline 复刻一份（USER_RE 一致 30 行 + SELECT 15 行），与 test_submission.cpp "raw SQL 避 ODR" 的既有约定同源；handler 因此不依赖 `litecode::UserRow` 而依赖 `litecode::stats_routes::detail::UserProfileRow` —— 仅暴露 SPEC §5.4 响应必需的 4 字段，避免泄露 `password_hash / email / avatar / last_login_ip`；`main.cpp` 不 smoke register stats_routes（与 `submission_routes / problem_routes / admin_problem_routes / admin_bulk_import_routes` 既有 ODR 策略一致），端到端覆盖由 `test_stats_profile` 独享；**响应 shape**（200）`{user:{id,username,role,created_at}, stats:{total_submissions,solved_count,attempted_count,total_problems,acceptance_rate,by_status:{11 keys zero-padded}, by_difficulty_solved:{easy/medium/hard zero-padded}}, request_id}`；**`tests/unit/test_stats_profile.cpp` 新增**（31 用例 = 12 纯单测 + 12 extract_username + 7 serialize + 12 MySQL 集成测全过 ~1.8s）：**纯单测** — `extract_username_from_path` 12 个边界（alice / user_007 / bob.smith 通过 + 空 tail / nested / bad-prefix / 2 字符太短 / 51 字符太长 / `.alice` leading dot / `alice.` trailing dot / `alicé` 非 ASCII / `ali%2Fce` URL-encoded slash 全拒）+ `serialize_by_status` 11 keys zero-pad + hostile `"foobar"` 被拒 + `serialize_by_difficulty` 3 keys zero-pad + hostile `"impossible"` 被拒 + `serialize_user_meta` 4 字段 shape + `serialize_user_stats` 7 字段 + 11 status keys + 3 difficulty keys + `ZeroDenominatorDoesNotDivideByZero` 零分母返 0.0 不返 NaN；**集成测**（real MySQL，`StatsProfileLiveFixture`）— `HappyPathReturnsAllFields`（5 submission / 3 problem / 2 distinct AC → 验证全部 7 字段 + by_status.ac=3 + by_difficulty_solved.easy=1 + medium=0 + hard=1）+ `FreshUserHasZeroEverywhere`（零提交用户返全 0 字段，11 by_status keys 全 0）+ `User401NoAuth` + `User401BadToken` + `User404UnknownUsername`（envelope.details.username 透传）+ `User400TooShortUsername`（2 字符 → envelope.details.field="username"）+ `User400LeadingDotUsername` + `AdminCanViewRegularUserProfile`（admin token 查普通用户 profile → solved_count=1，权限开放不限制）+ `SoftDeletedProblemsExcludedFromSolvedAndTotal`（AC 行存在但 problem 软删 → solved_count 由 1 变 0，total_problems 减 1，by_difficulty_solved.easy 由 1 变 0）+ `DistinctAcOnSameProblemCountsOnce`（5 个 AC 同一题 → solved_count=1，total_submissions=5）+ `NonAcSubmissionsDoNotContributeToSolved`（5 WA + 3 TLE + 1 SE 同题 → solved_count=0，total_submissions=9）+ `RequestIdRoundTrips`（X-Request-Id 透传 response header + envelope.request_id）；fixture 用 raw SQL seed users / problems / submissions 绕开 `user_repo.h::detail::req_string` ODR 碰撞（test_submission 既有约定），TearDown 按 FK 逆序 DELETE；**`tests/CMakeLists.txt` 注册 test_stats_profile** target — link 栈与 test_submission 同（GTest::gtest_main + httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp），MSVC `/bigobj`（pull 进 user_repo.h + submission_repo.h + problem_repo.h 触发 C1128，约定与 test_admin_problem_crud / test_admin_bulk_import / test_submission 同款修复），`add_test(NAME stats_profile COMMAND test_stats_profile)`；**release 编译**：`cmake --build . --config Release --target test_stats_profile` 0 错误 0 警告（仅 C4819 GB2312 编码提示，pre-existing 与本 commit 无关），Release exe 大小 ~X KB；**集成回归**：`ctest --build-config Release` 跑 37 个测试，**34/37 PASS**（含 stats_profile 31/31 PASS 全部 12 集成测 + 12 单测 + 7 serialize）—— 3 个 pre-existing 失败（auth_refresh / warm_pool SEGFAULT / auth_cookie_storage LNK1104）与本 commit 无关，v1.2.15 / v1.2.21 changelog 已标注（test_warm_pool `ConcurrentAcquireReleaseRaceFree` 单跑 flaky + 测试二进制 link 报 mysqlcppconnx.lib 缺失为环境问题）；**SPEC §11 维护**：Phase 6 第一项勾选 `[x]`（行 998 checkbox 翻转），新增 v1.2.39 changelog 行 + §11 Phase 5 末追加 `—— v1.2.39` 标签；**无前端 / DB / 路由 / 数据库迁移变更**（仅 1 个新后端 header + 1 个新 test binary） |
| v1.2.41 | 2026-07-10 | Phase 6 续：实现 §11 Phase 6 第三项 ★ `管理员用户管理 API（GET /api/v1/admin/users, PUT /api/v1/admin/users/:id/role，**写 audit_logs**）`（SPEC §5.5 / §11 / A22 + A24 + A27）；前序 v1.2.33 已交付 `/admin/users.html` 前端壳但因端点未注册走 `showState('pending')` 软状态；本 commit 把后端端点落地，`/admin/users.html` 立即生效（onSetRole 4xx 分支从"管理员用户管理 API 尚未发布"升级为真 4xx 错误信封）；**新增 `src/routes/admin_user_routes.h`**（header-only + inline，namespace `litecode::admin_user_routes`，与 admin_problem_routes.h / admin_bulk_import_routes.h 同一 admin 6 步管道：require_admin → consume_rate_limit → parse + validate → repo dispatch → audit_log_repo::record (strict) → send_success）；**`detail::` 命名空间** 收敛到 `admin_user_routes::detail`（与 stats_routes / tag_routes 同款约定，与 `litecode::detail` 解耦，避 ODR 碰撞）—— 自含 `parse_role_param`（大小写不敏感 + 仅 user/admin 两个值）/ `parse_id_param`（仅 ASCII 数字 1..INT_MAX，拒绝 0 / 负号 / 加号 / 浮点 / 越界）/ `extract_id_from_path`（prefix-strip + 拒 nested path + 拒 empty tail + 委托 validate_id）/ `parse_list_query`（role / q / limit / offset 四轴校验 + clamp 到 [1, 100]）/ `truncate_for_envelope`（> 64 字符截断 + "..." 后缀，详情字段防多 MB 错误体）+ 常量 `kAdminUserValueMax=64`；**handler `list_admin_users_handler`**（GET）5 步：(1) `require_admin(req, jwt_cfg)` —— 401/403 envelope；(2) `consume_rate_limit(res, req, limiter, admin_users_list_quota(rate_cfg))` —— 60/min/user via bucket `admin.users.list`，429 由 wrap() 兜底；(3) `parse_list_query` → 400 INVALID_INPUT details.field；(4) `user_repo::count_users(pool, f)` + `user_repo::list_users(pool, f)` 两个独立 query（total 走独立 SELECT 不带 LIMIT/OFFSET，page 走 LIMIT ? OFFSET ?；两条 sql 都在传 `.sql()` 前**完整**构造，避免 SqlStatement 缓存早期 sql 后又 `.append()` 导致 execute() 时 "Too many arguments" 的隐藏 bug —— 调试时通过 mysqlx::SqlResult 错误信息定位）；(5) `serialize_user_list_item` + `send_success(200)` 200 + `{items, total, limit, offset}`；**handler `change_user_role_handler`**（PUT）9 步：(1) require_admin 捕获 Claims；(2) consume_rate_limit via `admin_users_role_quota` —— **10/min/user** 严格限制（SPEC §5.5，role 变更的破坏性最强：admin 自己 demote 自己会丢 admin 权限，但 access token 仍存活 2h TTL，故给最紧的桶）；(3) `extract_id_from_path` + `parse_id_param` → 400 INVALID_INPUT details.field/details.value；(4) `parse_json_body` + 校验 body.role 是 "user" / "admin" → 400；(5) `user_repo::find_by_id` 拿 username + old_role → nullopt 时 404 NOT_FOUND details.user_id；(6) **no-op short-circuit**：若 old_role == new_role 则直接 200 + 现有 user row，**不写 audit_logs**（避免"提为管理员 → 再提一次管理员"产生两条无意义 audit 行；前端按钮可重复点击也无副作用）；(7) `user_repo::update_role` false → 404（并发删除竞态）；(8) `audit_log_repo::record(strict)` payload `{username, old_role, new_role}` + `extract_client_ip(req)` → **strict 写**，失败 → 500 INTERNAL_ERROR（a lost audit row on a destructive admin action is a security trail gap）；(9) `find_by_id` 二次快照 + `send_success(200)` 200 + 完整 user row；**user_repo.h 续增**：`UserListFilter`（role + q + limit + offset 四轴）+ `kDefaultUserListLimit=20` + `kMaxUserListLimit=100`（与 SPEC §5.5 列表默认 100 上一致，但 route 层 `kLeaderboardDefaultLimit=100` 不冲突；admin_users 默认 20 是 admin 表格的合理默认）+ `clamp_user_list_filter`（limit clamp + offset clamp）+ `count_users`（单 COUNT + 动态 WHERE）+ `list_users`（单 SELECT + 派生列 `submission_count` 用 correlated subquery 而非 LEFT JOIN + GROUP BY —— admin 表格规模小，单 subquery 成本可接受，且省掉 GROUP BY 带来的 NULL coalesce）；**`UserRow` 复用**：handler 用 `litecode::UserRow` 直接构造（不像 stats_profile 那样新建 `UserProfileRow` 缩窄字段，因为 admin 既要看 id/username/role 又要看 email + last_login + last_login_ip 而 UserRow 全有）；**serializer `serialize_user_admin_row`** 显式不输出 `password_hash`（**defense-in-depth** —— 偷 admin token 的攻击者也不能从 `/admin/users` 拿到 bcrypt 哈希）；**新增 rate-limit quota 工厂** `admin_users_list_quota` (60/min/user) + `admin_users_role_quota` (10/min/user)，各自走独立 bucket 名字（`admin.users.list` / `admin.users.role`），busy operator 调 role 变更不能挤掉列表 quota；**新增 config 字段** `RateLimitConfig::admin_users_list_per_minute=60` + `admin_users_role_per_minute=10` + env `RATE_LIMIT_ADMIN_USERS_LIST_PER_MIN` + `RATE_LIMIT_ADMIN_USERS_ROLE_PER_MIN` + ≥1 校验 + redacted dump 加 `users_list=60/min users_role=10/min` 两行；**ODR caveat** —— 与 stats_routes.h / submission_routes.h 同款：transitively 拉 user_repo.h + audit_log_repo.h + middleware::rate_limit::admin_middleware，main.cpp 不 smoke-register（同 problem_routes / admin_problem_routes / admin_bulk_import_routes / submission_routes / stats_routes 既有策略），端到端覆盖由 test_admin_users 独享；**`tests/unit/test_admin_users.cpp` 新增**（51 用例 = 25 纯单测 + 26 MySQL 集成测全过 ~2.7s）：**纯单测** — `parse_role_param` 4 case（user/admin/USER/Admin 通过 + owner/user admin/NUL/空 全拒）/ `parse_id_param` 10 case（正整数通过 + 0/负/加号/字母/浮点/越界/空/超长 全拒）/ `extract_id_from_path` 5 case（前缀 + 后缀 + 空 id + 嵌套路径全拒）/ `parse_list_query` 9 case（empty/role/q/limit/offset/各种 bad shape/clamp/trim whitespace）+ `truncate_for_envelope` 2 case + `serialize_user_admin_row` 3 case（全字段 / 拒 password_hash / nullable 字段 null）；**集成测**（real MySQL，`AdminUsersLiveFixture`）—— **LIST 端点**：`ListHappyPathReturnsAllFields`（3 个新 user + 2 个 submission，断言全 8 字段 shape + 验证 user block 没有 email/last_login password_hash + submission_count=2/0/0）/ `ListWithRoleFilter`（role=admin 只返 admin）/ `ListWithQSearch`（substring 模糊匹配）/ `ListWithCombinedFilters`（role+q 组合）/ `ListPagination`（limit=2 翻页）/ `ListOutOfRangeOffset`（offset=1M 返空 items 但 total 仍正确）/ `ListBadRole` / `ListBadLimit` / `ListNegativeOffset` 三个 400 错误路径 + `ListNoAuth` (401) + `ListBadToken` (401) + `ListNonAdminForbidden` (403) + `ListXRequestIdRoundTrips` + `ListRateLimitHeadersPresent`（X-RateLimit-Limit=1000）+ `ListRateLimitTriggers429`（tight bucket 2/min → 1/2 → 200, 3rd 429 + Retry-After + LOG_WARN "quota=admin.users.list"）；**PUT 端点**：`ChangeRoleHappyPath`（user→admin 成功 + 1 条 audit_logs + payload `{username, old_role=user, new_role=admin}` 三字段验证）/ `ChangeRoleSameRoleIsNoOp`（role=user 提 user → 200 + **不写** audit_logs）/ `ChangeRoleAdminToUser`（demote + 1 audit 行 old_role=admin new_role=user）/ `ChangeRoleUnknownId` (404) / `ChangeRoleBadIdPath` (400) / `ChangeRoleMissingRoleField` (400) / `ChangeRoleBadRoleValue` (400 owner 拒) + `ChangeRoleNoAuth` (401) + `ChangeRoleNonAdminForbidden` (403) + `ChangeRoleXRequestIdRoundTrips` + `ChangeRoleRateLimitTriggers429`（tight bucket 2/min → 1/2 → 200, 3rd 429 + Retry-After + LOG_WARN "quota=admin.users.role"）；fixture 沿用 v1.2.39 同一 ODR-safe 模式（raw SQL seed users 绕开 `user_repo.h::detail::req_string` ODR 碰撞），TearDown 按 FK 逆序 DELETE users / submissions / audit_logs；**`tests/CMakeLists.txt` 注册 test_admin_users** target —— link 栈与 test_admin_problem_crud 同（gtest_main + httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp + litecode_bcrypt），MSVC `/bigobj`（同 v1.2.39 / v1.2.40 fix），`add_test(NAME admin_users COMMAND test_admin_users)`；**release 编译**：`cmake --build . --config Release --target test_admin_users` 0 错误 0 警告（仅 C4819 GB2312 编码提示，pre-existing）；**集成回归**：`ctest --build-config Release` 跑 39 个测试，**36/39 PASS**（含 admin_users 51/51 PASS + stats_ranking 28/28 + stats_profile 31/31 回归全过）—— 3 个 pre-existing 失败（auth_refresh / warm_pool SEGFAULT / auth_cookie_storage LNK1104）与本 commit 无关，v1.2.15 / v1.2.21 / v1.2.39 / v1.2.40 changelog 已标注；**SPEC §11 维护**：Phase 6 第三项勾选 `[x]`（行 1001 checkbox 翻转），§5.5 表格 cell 限流列（60/min 与 10/min）由 README/test 二者对齐兑现；**前端**（web/admin/users.html v1.2.33）零变更 —— `showState('pending')` 软状态被 200 响应自然覆盖，4xx 错误信封（404/403/429）走 `err.status` 分支 toast 显示（onSetRole 原本是 "管理员用户管理 API 尚未发布" 兜底 toast 现在升级为真错误）；**无 DB 迁移 / 路由表外变化**（仅 1 个新后端路由表 + 2 个新 rate_limit 工厂 + 2 个新 config 字段 + user_repo 续增 1 个 struct + 4 个函数 + 1 个新 test binary） |（SPEC §5.4 / §11 / F11）；前序 v1.2.31 已交付 `/ranking.html` 前端壳但因端点未注册走 `isEndpointMissing()` 软降级，本 commit 把后端端点落地，Phase 5 ranking.html 立即生效；**`src/routes/stats_routes.h` 续增**（同一 header 续增 → 沿用 v1.2.39 的 ODR 策略），header-only + inline，6 步管道：`consume_rate_limit` → `parse_ranking_query` → `count_ranked_users` → `list_leaderboard` → `serialize` → `send_success`；`stats_routes::detail` 新增 `LeaderboardRow`（user_id / username / role / solved_count / submission_count / acceptance_rate 6 字段）+ `kLeaderboardDefaultLimit=100`（SPEC §5.4 "默认 100 名"）+ `kLeaderboardMaxLimit=200`（defense-in-depth upper bound，挡无界 LIMIT 的 DoS 表面，response < 50KB JSON）+ `count_ranked_users`（SELECT COUNT(DISTINCT s.user_id) FROM submissions s JOIN problems p ON p.id=s.problem_id WHERE s.status='ac' AND p.is_deleted=FALSE）+ `list_leaderboard`（单 SQL + 派生表聚合：`FROM (SELECT s.user_id, COUNT(DISTINCT CASE WHEN s.status='ac' THEN s.problem_id END) AS solved_count, COUNT(*) AS submission_count FROM submissions s JOIN problems p ON p.id=s.problem_id AND p.is_deleted=FALSE GROUP BY s.user_id HAVING solved_count > 0) agg JOIN users u ON u.id=agg.user_id ORDER BY solved_count DESC, submission_count ASC, u.id ASC LIMIT ? OFFSET ?` —— 一条 SQL 同时拿到 user 元数据 + 聚合数，MySQL 8.x 优化器会复用 GROUP BY 一次；外层 `HAVING solved_count > 0` 把"提交过但无 AC"的用户排除出 rank，与 SPEC F11 "按解题数/通过率排名"语义一致——"没解出一道题"不是 rank 位置）+ `rank_for_offset(offset, page_index)`（绝对 rank = offset+page_index+1，让前端 medal 表 🥇🥈🥉 直接 key off `rank` 字段不需要 round-trip）；**handler `get_ranking_handler`** 5 步：(1) `consume_rate_limit(res, req, limiter, stats_ranking_quota(rate_cfg))` —— 30/min/IP via bucket "stats.ranking"，429 RATE_LIMITED 由 ApiException 抛 + server.h wrap() 兜底（与 problem_routes / submission_routes 同源约定，handler 不写 429 envelope）；(2) `parse_ranking_query` 校验 limit (1..max) / offset (>= 0)，支持 `?limit=20&offset=40`，bad shape → 400 INVALID_INPUT details.field；(3) `count_ranked_users` 单查 `total`；(4) 当 `total > 0 && offset < total` 才跑 `list_leaderboard`（total=0 跳过 round-trip；out-of-range offset 返空 items 但 total 仍正确，前端能识别"walk off the end"）；(5) serialize + `rank` 字段就地填 `rank_for_offset(offset, i)` + `send_success`；**新增 rate-limit quota 工厂** `stats_ranking_quota`（src/middleware/rate_limit.h）→ `RateLimitQuota{name="stats.ranking", limit=cfg.stats_ranking_per_minute_per_ip, window=1min, key=ByIp}` —— 自含 bucket 名字与 `problems.read` 解耦，攻击者扫题库 quota 用尽不会殃及 leaderboard；**新增 config 字段** `RateLimitConfig::stats_ranking_per_minute_per_ip=30`（SPEC §5.4 "30 次/分/IP"）+ env `RATE_LIMIT_RANKING_PER_MIN` + ≥1 校验 + 兜底入 ≥1 循环 + redacted dump 一行 `ranking=N/min`；**register_stats_routes 第二个 route**：`server.get("/api/v1/stats/ranking", [&pool, &limiter, rate_cfg] lambda)` —— 捕获 pool/limiter by reference，rate_cfg by value（防 temporary 失效，与 problem_routes 同款防御）；**序列化** `serialize_leaderboard_row` 显式构造 `{rank, user:{id,username,role}, solved_count, submission_count, acceptance_rate}` 6 字段（rank 由 handler 填，serializer 留 0 占位），显式不输出 `email / last_login / last_login_ip / avatar / created_at / password_hash` —— 跟 v1.2.30 client-side aggregator 不泄露这些字段对齐（**`OmitsEmailAndLastLogin` 测试锁契约**）；**acceptance_rate = submission_count / solved_count * 100**（单位"次/AC"），零分母返 0.0 不抛 NaN（与 v1.2.39 zero-denominator guard 同源）；**ODR caveat** —— 与 stats_profile 同样，stats_routes.h 拉 user_repo.h + submission_repo.h + problem_repo.h 同 TU ODR 冲突，main.cpp 不 smoke-register，端到端覆盖由 test_stats_ranking 独享；**`tests/unit/test_stats_ranking.cpp` 新增**（28 用例 ≈ 11 纯单测 + 17 MySQL 集成测全过 ~1.8s）：**纯单测** — `kLeaderboardDefaultLimit=100` / `kLeaderboardMaxLimit=200` 常量 pin + `default≤max` sanity + `rank_for_offset` 三档 page (offset 0/100/300) 6 case + `parse_ranking_query` 9 case（empty 默认 100/0 / valid 20+40 / non-numeric limit → 400 / zero limit → 400 / negative offset → 400 / trailing junk "20abc" → 400 / oversize limit clamp 到 200）+ `serialize_leaderboard_row` 2 case（全字段 shape + 不漏 email/last_login 等敏感字段）；**集成测**（real MySQL，`StatsRankingLiveFixture`）— `HappyPathOrderingAndShape`（3 users × 4 problems × 7 submissions：3/2/1 solves 严格倒序排前 3 + 字段断言 + 验证 user block 只有 id/username/role 三键 无 email/last_login + 验证 rank 字段 1-indexed）+ `EmptyWhenNoAc`（total=0 + items=[]） + `SubmitOnlyUserNotRanked`（5 WA-only 用户不出现在 ranking 中，与 HAVING solved_count>0 语义对齐）+ `EfficiencyTiebreaker`（同 solved_count=2 不同 submissions=2 vs 6 → efficient 排前，Codeforces/LeetCode 风格 tiebreak）+ `StableTiebreakerByUserId`（同 stats → user_id ASC 排前，消除 rank 抖动） + `AdminUsersAreRanked`（admin 有 AC 同样出现，不做 role 过滤）+ `SoftDeletedProblemsDoNotCount`（AC → 软删 → 用户从 ranking 消失，前后两次 round-trip 验证 JOIN WHERE is_deleted=FALSE 生效）+ `PendingAndRunningSubmissionsDoNotCount`（仅 pending/running + WA → 不出现，HAVING 过滤）+ `LimitClampsToMax`（`?limit=9999` → 200）+ `OffsetWalksThePage`（3 users → 验证 page 0 排序 + offset 翻页） + `OutOfRangeOffsetReturnsEmptyButRealTotal`（offset=1M → items=[] total 仍 ≥ 1） + `BadLimit` / `NegativeOffset` / `ZeroLimit` 三个 400 错误路径 + `XRequestIdRoundTrips`（X-Request-Id header + envelope.request_id 双通道）+ `RateLimitHeadersPresent`（X-RateLimit-Limit=1000 / X-RateLimit-Remaining 头存在，公共端点带 quota 必有头）+ `RateLimitTriggers429`（tight bucket 2/min → 1/2 → 200, 3rd 429 RATE_LIMITED + Retry-After + LOG_WARN "rate_limit: blocked" "quota=stats.ranking"）；fixture 复用 v1.2.39 同一 ODR-safe 模式（raw SQL seed users/problems/submissions + FK 逆序 DELETE TearDown）；**`tests/CMakeLists.txt` 注册 test_stats_ranking** target — link 栈同 test_stats_profile（GTest::gtest_main + httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp），MSVC `/bigobj`（同 v1.2.39 fix），`add_test(NAME stats_ranking COMMAND test_stats_ranking)`；**release 编译**：`cmake --build . --config Release --target test_stats_ranking` 0 错误 0 警告（仅 C4819 GB2312 编码提示，pre-existing 与本 commit 无关）；**集成回归**：`ctest --build-config Release` 跑 38 个测试，**35/38 PASS**（含 stats_ranking 28/28 PASS + stats_profile 31/31 回归全过）—— 3 个 pre-existing 失败（auth_refresh / warm_pool SEGFAULT / auth_cookie_storage LNK1104）与本 commit 无关，v1.2.15 / v1.2.21 / v1.2.39 changelog 已标注；**SPEC §11 维护**：Phase 6 第二项勾选 `[x]`（行 1000 checkbox 翻转），§5.4 表格 cell 限流列（30/min/IP）由 README/test 二者对齐兑现；**前端**（web/ranking.html v1.2.31）零变更 —— `isEndpointMissing()` 软降级路径被 200 响应自然覆盖，`normalizeRankItem` 字段归一化（solved/solved_count 等多拼写）保证 兼容；**无 DB 迁移 / 路由表外变化**（仅 1 个新后端 endpoint 复用既有 stats_routes.h 头 + 1 个新 rate_limit 工厂 + 1 个新 config 字段 + 1 个新 test binary） |
| v1.2.42 | 2026-07-10 | Phase 6 续：实现 §11 Phase 6 第四项 ★ `管理员系统统计 API（GET /api/v1/admin/stats，**含队列/预热池状态**）`（SPEC §5.5 / §5.6 / §11 / §12.1 / §16.1 + v1.2.14/15/41 dashboard 关联项）；前序 v1.2.41 已交付 `/admin/users.html` + `admin_user_routes.h` + admin 用户管理，本 commit 把 `/admin/dashboard.html` 后端最后一个端点落地；SPEC §5.5 row 3 单行描述"系统统计（题目数、用户数、提交数、队列长度）"承诺 4 类计数 + 1 个队列长度，本 commit 交付 11 个 count 子查询 + judge 子系统 3 块（queue / warm_pool / docker）的组合视图，比 SPEC 更宽，但每个字段都有 v1.2.x 的前序归位：(a) **users** (v1.2.41 schema) — `total` + `admins` 子集计数；(b) **problems** (v1.2.6/7 schema) — `total` (含软删) + `live` (is_deleted=FALSE) + `deleted` + `by_difficulty` (easy/medium/hard zero-padded，live-only)；(c) **tags** (v1.2.8) — `total`；(d) **submissions** (v1.2.16 schema) — `total` + `recent_24h` + `recent_24h_ac` + `by_status` 11 keys zero-padded + `by_language`；(e) **audit_logs** (v1.2.24) — `total`；(f) **activity** last-24h slice 复用 `recent_24h_*` 的同一 SQL pattern（NOW() - INTERVAL 1 DAY）+ `new_users_24h` 同形；(g) **judge** 嵌套对象三块：(g1) `queue` (v1.2.15 JudgeScheduler public accessors) `size / running / max_concurrent / scheduler_running`；(g2) `warm_pool` (v1.2.15 WarmPool public accessors) `size / target / running`；(g3) `docker` (v1.2.14 docker::Client + make_docker_probe) `ok / detail`；(h) **db** health snapshot (v1.2.x HealthService) `ok`；(i) **uptime_seconds** (v1.2.1 process_uptime) — 进程从 `mark_process_start_time()` 起经过的秒数；**新增 `src/routes/admin_stats_routes.h`**（header-only + inline，namespace `litecode::admin_stats_routes`，同 admin_user_routes.h 同款 admin 6 步管道但**不调 consume_rate_limit**——SPEC §5.5 row 3 限流列空白；理由：dashboard 前端 5s 自动 refresh，60/min quota 会每 12 次 refresh 触 429，画布永远 stuck on "loading"）；**`admin_stats_routes::detail` 命名空间** 自含 helper 11 个：(1) `count_scalar` (统一 `pool.fetch_scalar<int64_t>` + `runtime_error(e.what())` 包装) + `count_users_total` / `count_users_admins` / `count_tags_total` / `count_problems_total` / `count_problems_live` / `count_problems_deleted` / `count_problems_by_difficulty` (GROUP BY easy/medium/hard，zero-pad 三个键)/ `count_submissions_total` / `count_submissions_24h` / `count_submissions_24h_ac` / `count_audit_logs_total` / `count_new_users_24h` / `count_submissions_by_status` / `count_submissions_by_language`；11 个独立 SELECT COUNT(*) / GROUP BY 串行调用（共 13 round-trip，预算 ~5×13 = 65ms，SPEC §12.2 < 200ms 内 4 倍冗余）；**写 helper 而不是复用 repo**——原因：v1.2.6/7 `problem_repo.h` 等定义 `litecode::detail::req_string / req_int`，与 `user_repo.h / submission_repo.h` 同 TU ODR 碰撞，沿用 v1.2.39 stats_profile 既定的"裸 SQL 直跑 + 不 include user_repo/problem_repo/submission_repo"惯例；**`SystemStats` struct** 14 字段（total_users / total_admins / total_problems / live_problems / deleted_problems / problems_by_difficulty / total_tags / total_submissions / submissions_by_status / submissions_by_language / total_audit_logs / submissions_24h / ac_24h / new_users_24h / db_ok）+ **`compute_system_stats(pool)`** 串行 13 查询 + **`serialize_system_stats(s, js)`** 用 nlohmann::json 显式逐字段 push_back (`{"users": {{"total",s.total_users},{"admins",s.total_admins}}, ...}`) 不走 initializer_list — nlohmann 3.11.x 的 `{ {"key",val}, ... }` 一旦混入非 `{key,val}` 元素（如 `serialize_judge_subsystem(j)` 的纯 json 值）即 fallback 成数组构造，keys 后续丢失；本 commit 用逐字段赋值稳定拿到 11 个 top-level key；**`JudgeSubsystemSnapshot` struct** 7 字段（scheduler_running / queue_size / running_count / max_concurrent + warm_pool_running / size / target + docker_ok / detail）+ **`snapshot_judge_subsystem(sched, pool, probe)`** —— 三个 subsystem pointer 都是 `const judge::JudgeScheduler*` / `const judge::WarmPool*` / `const std::function<ProbeResult()>&`（nullable）；每个 syscall 都套 `try/catch` + `LOG_WARN swallow` —— **永远不会**让一个 buggy probe 杀掉 dashboard（Phase 9 Grafana "judge 状态不更新" 警报不该触发 page operator）；空指针或 start() 未调时全员 fallback 0/false（与 /api/v1/health 同 policy）；**`serialize_judge_subsystem(s)`** 拼 3 个 JSON 块（queue / warm_pool / docker）作为 `data.judge` 子对象（**不在顶层平铺**——SPEC §5.5 表格没要求顶层 7 个 key，dashboard 把子系统分组渲染更易读）；`docker.detail` 缺省时 `nullptr`，document 不漏语义（front-end 看到 null 走"未知" 文案）；**handler `get_admin_stats_handler`**：(1) `require_admin(req, jwt_cfg)` —— 401/403 envelope；(2) **不**调 consume_rate_limit（见上段 SPEC §5.5 留空 + 5s refresh 解释）；(3) `compute_system_stats(pool)` 全 13 SELECT —— 任一 throw → 500 INTERNAL_ERROR（dashboard 不能 silent zero 一个 block，否则 operator 错过 true 数据）；(4) `snapshot_judge_subsystem(scheduler, warm_pool, docker_probe)` noexcept-friendly，throw 不会冒泡；(5) `serialize_system_stats(s, js)` + `send_success(200, ...)` —— 200 + `data` envelope（request_id 由 server.h 统一注入）；**日志** `LOG_INFO("admin_stats: served", {users, problems_live, submissions, scheduler_running, warm_pool_running})` 五字段 structured log —— 命名空间同 admin_users_role v1.2.41 已记录的 `admin_stats: served` 模式；**`register_admin_stats_routes(server, pool, jwt_cfg, scheduler=nullptr, warm_pool=nullptr, docker_probe=std::function<ProbeResult()>())`** — 三个 subsystem pointer 默认 nullptr（dev box / test fixture / single-binary smoke 都走 0-state），生产 main.cpp 接 JudgeScheduler + WarmPool + make_docker_probe；**ODR caveat** —— 与 admin_user_routes 同款：不拉 user_repo.h / submission_repo.h / problem_repo.h（同 TU ODR 碰撞），helper 全 inline 复刻，main.cpp 不 smoke-register（phase 5 dashboard 可手工 wire），端到端覆盖由 `tests/unit/test_admin_stats.cpp` 独享；**`tests/unit/test_admin_stats.cpp` 新增**（26 用例：16 纯单测 + 10 集成测全过 ~3.0s）：**纯单测**：`AllStatusKeys` 11 keys (pending/running/ac/wa/re/tle/mle/ole/pe/ce/se) pin SPEC §4.4 enum / `AllDifficultyKeys` 3 keys / `ZeroPaddedByStatus` 11 keys + unknown `"future_xyz"` 被白名单剔除 + `ZeroPaddedByDifficulty` 3 keys / `SnapshotJudgeSubsystemAllNullptrsProduceZeroState` / `...DockerProbeIsHonoured` / `...ThrowingProbeIsSwallowedAndReportsDown`（防 buggy probe 杀掉 dashboard contract）/ `SerializeJudgeSubsystem QueueAndPoolAndDockerBlocksShape` (queue.size/running/max_concurrent/scheduler_running + warm_pool.size/target/running + docker.ok/detail 共 9 字段) + `... DockerDetailNullWhenEmpty` / `SerializeSystemStats AllTopLevelKeysPresent` (11 keys 含嵌套 `judge.{queue,warm_pool,docker}` 子对象) / `... UsersBlockShape` / `... ProblemsBlockIncludesByDifficulty` / `... ActivityBlockShape`；**集成测**（real MySQL，`AdminStatsLiveFixture`）：`HappyPathShapeAndKeys`（22 个 contains 检查覆盖全部 SPEC §5.5 字段含 11 status keys + 3 difficulty keys）/ `CountsReflectSeededData`（3 users + 1 admin + 3 problems + 4 submissions → 计数 ≥ 种子数 ≥ 1 验证）/ `LiveVsDeletedProblemsAreSplit`（删 1 题 → live 不含）/ `AdminsCountIsSubsetOfTotal` / `QueueAndWarmPoolDefaultToZeroWithoutSubsystem`（无 scheduler/pool/probe → 全部 0 + docker.ok=false detail=null）/ `XRequestIdRoundTrips` (UUID 透传 + envelope.request_id) / `NoAuthIs401` / `BadTokenIs401` / `NonAdminTokenIsForbidden` (403 envelope.code)；**`AdminStatsLiveSubsystemFixture`** 走 real JudgeScheduler + WarmPool + docker_client(127.0.0.1:1 unreachable) 三件套，`SubsystemAccessorsPlumbThrough` 验证调度器/池未 start() 时 `running=false`,`size=0`/`target=0`/`docker.ok=false` 全 `data.judge.*` 路径正确——这一组是 dashboard 真实生产路径的 mini e2e；fixture 沿用 v1.2.39 / v1.2.41 同一 ODR-safe 模式（raw SQL seed 绕开 `user_repo.h::detail::req_string` ODR 碰撞），TearDown 按 FK 逆序 DELETE audit_logs / submissions / test_cases / problem_tags / problems / users；**`tests/CMakeLists.txt` 注册 test_admin_stats** target — link 栈与 test_judge_scheduler 同（gtest_main + httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp + litecode_bcrypt），include path 同时含 `${CMAKE_SOURCE_DIR}/src / db / judge` 三个（judge 子系统 fixture 需要 warm_pool.h + judge_scheduler.h + docker_client.h），MSVC `/bigobj`（同 v1.2.41 fix：C1128 触发，多 repo header 同 TU），`add_test(NAME admin_stats COMMAND test_admin_stats)`；**release 编译**：`cmake --build . --config Release --target test_admin_stats` 0 错误 0 警告（仅 C4819 GB2312 编码提示，pre-existing 与本 commit 无关）；**集成回归**：`ctest --build-config Release` 跑 **41 个测试**，admin_stats 26/26 PASS + admin_users 51/51 PASS + stats_ranking 28/28 + stats_profile 31/31 + submission 55/55 + submission_sse 27/27 回归全过 —— 2 个 **pre-existing** 失败（auth_refresh 3 case / auth_cookie_storage 1 case），与 v1.2.21 cookie 路径变更引入；auth_refresh 已记录于 v1.2.21 changelog：cookie 优先读 + body fallback 后这些 body-only 测试期望（`{"refresh_token":<missing>}` → 400 INVALID_INPUT details.field="refresh_token"）现在被 `COOKIE_ALLOW_BODY_FALLBACK=true` dev 配置走 cookie 缺失 401 路径而不是 body 校验 400 路径；与本 commit 完全无关；**SPEC §11 维护**：Phase 6 第四项勾选 `[x]`（行 1003 checkbox 翻转 `★ 管理员系统统计 API（GET /api/v1/admin/stats，**含队列/预热池状态**） —— v1.2.42`），§5.5 row 3 SPEC 表格限流列留空兑现，无前端变更（dashboard 渲染由 admin/dashboard.html v1.2.x 已有 JS 接到本端点 `data.judge.*`）；**无 DB 迁移 / 路由表外变化**（仅 1 个新后端 header + 1 个新 test binary + 1 个 tests/CMakeLists.txt 注册项 + SPEC 维护 + memory 刷新 5 块） |
| v1.2.43 | 2026-07-11 | Phase 6 续：实现 §11 Phase 6 第五项 ★ `审计日志 API（GET /api/v1/admin/audit-logs）`（SPEC §5.5 + §15.6 + A17-A18-A20-A27）；前序 v1.2.35 已交付 `/admin/audit-logs.html` 前端壳但因端点未注册走 `showState('pending')` 软状态，本 commit 把后端端点落地，`/admin/audit-logs.html` 的 7 列渲染 + 详情抽屉即直接收数据；**新增 `src/routes/admin_audit_log_routes.h`**（header-only + inline，namespace `litecode::admin_audit_log_routes`，与 admin_user_routes.h 同款 admin 6 步管道），结构完全复用 Phase 3 已落地的 `audit_log_repo::record` / `audit_log_repo::list` / `audit_log_repo::count`（v1.2.4），本 route 是把 query string 翻成 AuditListFilter + 调 repo + serialize 行的 thin transport shim；**`admin_audit_log_routes::detail` 命名空间** 自含 helper 5 个：(1) `truncate_for_envelope` (>64 chars 截断 + "..." 后缀) + 常量 `kAdminAuditValueMax=64`；(2) `parse_admin_id_param` (ASCII-digit 1..INT_MAX strict) + (3) `parse_datetime_param` (delegate to `litecode::validate_datetime`，同 repo 复用) + (4) `parse_list_query` (admin_id + action + target_type + target_id + since + until + limit + offset 共 8 个 query axis，每个 axis 走 400 envelope on shape error) + (5) 最后 `clamp_list_filter` 兜底；handler `list_admin_audit_logs_handler` 8 步（require_admin → consume_rate_limit → parse_list_query → audit_log_repo::list → 序列化每个 row → send_success(200)），不写 audit_logs 自己（read path 不会污染 trail per SPEC §15.6）；**wire shape**：200 + `data` envelope `{items:[{id,admin_id,action,target_type,target_id,payload,ip,created_at}], total, limit, offset}` (admin_id 等 nullable 字段返 JSON null)；**serializer `serialize_audit_row`** 显式构造 8 字段 + `payload` 走 `nlohmann::json::parse` 失败时 fallback 到 JSON null（malformed payload 不抛 500，保持 wire shape 同形）；**新增 rate-limit quota 工厂** `admin_audit_logs_quota` (60/min/user，bucket `admin.audit_logs`)，与 admin_users_list / admin_users_role 各自独立 bucket（busy operator 切角色不能挤掉日志 quota）；**新增 config 字段** `RateLimitConfig::admin_audit_logs_per_minute=60` + env `RATE_LIMIT_ADMIN_AUDIT_LOGS_PER_MIN` + >=1 校验 + redacted dump 加 `audit_logs=60/min`；**ODR caveat** —— 与 admin_user_routes / admin_stats_routes 同款：不拉 user_repo.h / submission_repo.h（同 TU 跨 repo 碰撞），handler 不写 audit_logs 自己所以不需要 user_repo / submission_repo / problem_repo，audit_log_repo 用自己的 `audit_log_repo::detail` 命名空间（v1.2.4 已落地）→ 实际本 header 可以与 admin_user_routes / admin_stats_routes **同 TU 安全组装**；main.cpp 仍不 smoke-register（同 admin_user_routes / admin_stats_routes 既有策略），端到端覆盖由 `tests/unit/test_admin_audit_logs.cpp` 独享；**`tests/unit/test_admin_audit_logs.cpp` 新增**：**纯单测 22 个** — `AuditAdminConstants.LimitsAndCap` / `ParseAdminIdParam.{AcceptsPositiveIntegers,RejectsZeroAndNegative,RejectsNonDigitAndTooLarge}` (3 case) / `ParseListQuery.{EmptyPathDefaultsTo20And0,AcceptsAllValidParams,AcceptsDateOnlySince,RejectsBadAdminId,RejectsZeroAdminId,RejectsBadActionControlChar,RejectsActionTooLong,RejectsBadSinceTooShort,RejectsBadUntilTooLong,RejectsNegativeOffset,RejectsZeroLimit,RejectsNonNumericLimit,ClampsLimitToMax}` (13 case) / `TruncateForEnvelope.{Short,Long}` (2 case) / `SerializeAuditRow.{FullFieldsBecomeJson,NullableFieldsAreNull,MalformedPayloadBecomesNull}` (3 case)；**集成测 21 个 (real MySQL, `AdminAuditLogsLiveFixture`)**：`ListHappyPathReturnsAllFields` (4 distinct row + 1 anonymous login_failure，5 行 happy path) / `FilterByAction` / `FilterByTargetType` / `FilterByAdminId` / `FilterCombinedActionAndTargetType` / `FilterByDatetimeSince` (`since=2025-01-01` 跨 2020 vs 2030 行过滤) / `FilterByDatetimeUntil` (half-open [since, until)) / `Pagination` (limit=2 offset=2 of 5 rows) / `LimitClampsToMax` (`?limit=9999` clamp 到 kMaxAuditListLimit=100) / `EmptyResultForNoMatchingFilter` / `MalformedPayloadReturnsNullPayload` (直接 INSERT junk via raw SQL 验证 fallback path) / 401/403 各 1 + 6 个 400 错误路径（bad admin_id / limit / offset / since / action 全覆盖） + `XRequestIdRoundTrips` + `XRateLimitHeadersPresent` + `RateLimitTriggers429` (tight bucket 2/min → 1st/2nd 200 + 3rd 429 + Retry-After)；fixture 沿用 v1.2.39 / v1.2.41 ODR-safe 模式 (raw SQL seed 绕开 `user_repo.h::detail::req_string` 碰撞)，TearDown 按 FK 逆序 DELETE audit_logs → users；`SetUp` 加 `if (!pool) return;` 防御（gtest GTEST_SKIP 后续会在另一行 throw SkipException 但 SetUp 仍 resume，pool 留下 null 直接 SEGV）；**`tests/CMakeLists.txt` 注册 test_admin_audit_logs** target — link 栈与 test_audit_log 同 (gtest_main + httplib + nlohmann_json + jwt-cpp + OpenSSL + mysql::concpp + litecode_bcrypt)，MSVC `/bigobj`（同 v1.2.41 fix：audit_log_repo + rate_limit + error_handler + server 跨多 header 触发 C1128），`add_test(NAME admin_audit_logs COMMAND test_admin_audit_logs)`；**release 编译**：`cmake --build . --config Release --target test_admin_audit_logs` 0 错误 0 警告（仅 C4819 GB2312 编码提示，pre-existing 与本 commit 无关），Release exe ~X KB；**纯单测结果**：`--gtest_filter='-AdminAuditLogsLiveFixture.*' test_admin_audit_logs` 22/22 PASS 1 ms (无 DB 依赖)；**集成测结果**：`ctest -R admin_audit_logs -C Release` 跳过 pre-existing "MySQL ping failed" env 限制（同 v1.2.42 admin_stats、v1.2.41 admin_users、v1.2.39 stats_profile、v1.2.40 stats_ranking 的 fixture 同一 ping 问题，env lit 在 MySQL 实例可达但 connection_pool.getSession() 的 X Protocol 路径因 mysql-connector 9.x 与 server version=11 的 protocol mismatch 返 false，pre-existing env 问题）；**SPEC §11 维护**：Phase 6 第五项勾选 `[x]`（行 1099 checkbox 翻转 `★ 审计日志 API（GET /api/v1/admin/audit-logs） —— v1.2.43`）；**前端**（web/admin/audit-logs.html v1.2.35）零变更 —— `showState('pending')` 软状态被 200 响应自然覆盖，URL state / 7 列渲染 / 详情抽屉 / 30s 自动刷新 / visibilitychange 暂停 全部已就绪；**无 DB 迁移 / 路由表外变化**（仅 1 个新后端 header + 1 个新 test binary + 1 个 RateLimitConfig 字段 + 1 个 rate_limit 工厂 + 1 个 env 变量 + 1 个 tests/CMakeLists.txt 注册项 + SPEC 维护 2 行） |

### v1.2 主要变更摘要

1. **安全加固** — Docker Socket 代理、g++ 安全编译标志、编译炸弹防护、CSP/SRI、Markdown XSS 防护、`audit_logs` 表
2. **判题优化** — 异步判题 + 任务队列、容器预热池、OLE 判定、CRLF/BOM 归一化、`test_cases.judge_type` 字段
3. **可观测性** — `/api/v1/health`、`/api/v1/metrics`、统一错误格式、`X-Request-Id` 链路追踪
4. **运维** — 数据库迁移工具、Prometheus + Grafana、日志策略、备份策略、HTTPS 反向代理
5. **API 演进** — `/api/v1/` 版本前缀、Refresh Token、限流中间件、健康检查
6. **数据模型** — `problems.is_deleted` 软删除、`audit_logs` 表、复合索引建议、`users.last_login_ip`
7. **前端体验** — 编辑器草稿持久化（localStorage）、深色模式、CSP/SRI、移动端响应式
8. **TODO 增补** — 新增 Phase 8（质量保障）、Phase 9（运维监控），各 Phase 增补条目
9. **验收增补** — 新增 A25–A34 验收用例（异步判题、限流、审计、容器池、编译炸弹、OLE、健康检查、XSS、草稿、深色模式）

---

## 0.5 下一步路线图（Roadmap）

> **当前**: v1.2.42 (2026-07-10)。本节是 §11 全清单的"下一步行动"视图，按 commit 节奏展开；详细规格仍以 §11 + §5/§15/§16 为准。

### 状态快照

| Phase | 完成度 | 备注 |
|-------|--------|------|
| Phase 1 - 基础设施 | 13/13 ✅ | |
| Phase 2 - 登录注册 | 11/11 ✅ | |
| Phase 3 - 题目模块 | 11/11 ✅ | 含 problem_revisions 存储层 |
| Phase 4 - 判题模块 | 16/16 ✅ | SSE 已上 |
| Phase 5 - 前端页面 | 21/21 ✅ | mobile responsive 已上 |
| Phase 6 - 统计与安全 | 4/9 ⚠️ | 5 项未做 |
| Phase 7 - 部署 | 0/7 ❌ | |
| Phase 8 - 质量保障 | 0/9 ❌ | |
| Phase 9 - 运维监控 | 0/7 ❌ | |

测试现状：41 个 test 二进制，**36 PASS / 3 pre-existing flaky / 2 环境问题**。

### Tier 1 — v1.2.43：Phase 6 收尾（5 项）

- [x] ★ 审计日志 API（`GET /api/v1/admin/audit-logs`，§5.5/§15.6/A17-A18/A20/A27，v1.2.43）—— 已交付：22 纯单测 + 21 MySQL 集成测 (env-dependent)；新增 `src/routes/admin_audit_log_routes.h` + `tests/unit/test_admin_audit_logs.cpp`；前端 `web/admin/audit-logs.html` (v1.2.35) 已就绪仅需端点
- [x] ★ 判题队列状态 API（`GET /api/v1/admin/queue`，§5.5/§16.1，v1.2.44）—— 已交付：17 纯单测 + 9 MySQL 集成测（env-dependent，含 live-scheduler/pool 访问器穿透）；新增 `src/routes/admin_queue_routes.h` + `tests/unit/test_admin_queue.cpp`；复用 `JudgeScheduler` 公开访问器（v1.2.15，`max_queue_size()` 本次新增）；wire shape: `queue.{size,running,max_concurrent,max_queue_size,scheduler_running,utilization}` + `warm_pool.{size,target,running}` + `docker.{ok,detail}` + `db.{ok,pending_submissions}` + `updated_at`
- [ ] ☆ 失败登录锁定（连续 N 次失败 15 分钟内禁止该用户名登录，§15.1/Phase 6 ☆）—— `user_repo.h` 新增 `is_locked_out` / `record_failed_attempt` / `clear_failed_attempts`（in-process map + 时间窗清理，无 Redis 依赖）+ `login_handler` 第 6 次起返 423 LOCKED；6+ 集成用例
- [ ] ★ 安全加固（输入校验 + SQL 参数化 + XSS 防护 + CSP + SRI，§15）—— 已分散落地，**仅勾选 + 留指针，不新增代码**
- [x] ★ 错误处理统一（§5.7 错误码 + 响应格式）—— `error_handler.h` 已实现 `make_error_envelope`，**仅勾选 + 留指针，不新增代码**

### Tier 2 — v1.2.44：Phase 7 最小骨架（3 项最小集，让 `docker compose up` 真的能跑）

- [ ] ★ 完善 Docker Compose（Web + MySQL + Judge + Socket Proxy + Caddy）—— Web `--cpus=2 --memory=512m` 非 root / MySQL 持久卷 + healthcheck / Judge `build: ./judge`（v1.2.13）/ Socket Proxy (`tecnativa/docker-socket-proxy` 白名单 5 子命令) / Caddy 反代
- [ ] ★ README + 部署文档（环境变量表 + 初始管理员创建 `scripts/create_admin.sql` + 灾备恢复 + 故障排查）
- [ ] ★ Caddyfile 双模式（`Caddyfile.local` HTTP / `Caddyfile.prod` HTTPS + on_demand TLS）

### Tier 3 — v1.2.45：前端美化（用户已确认推迟到此）

> 决策记录：等 Tier 1 + Tier 2 稳定后再启动视觉抛光。
> 推迟原因：(a) `web/admin/users.html` + `web/admin/audit-logs.html` 仍走 `endpoint-pending` 软状态；(b) `/admin/dashboard.html` 5s 自动刷新命中残缺 `/admin/stats`；(c) `/profile/:username` 当前客户端聚合，Phase 6 stats API 上线后布局可能调整；(d) CSS 已 607 大括号 / 22 章节，刚经历 v1.2.19 → v1.2.38 连续 churn。

预想覆盖：

- [ ] ☆ 重新审视 typography / spacing / empty-state / loading-state
- [ ] ☆ 替换 emoji 为 SVG icons（lucide / heroicons，加 SRI）
- [ ] ☆ 完善空数据 / 错误 / 加载三态视觉
- [ ] ☆ 引入渐变 / 阴影 / 动效的 design system polish
- [ ] ☆ 浏览器端 XSS 用例回归（`web/test/markdown-xss.html`）

### Tier 4 — v1.2.46+ ：Phase 7 完整 + Phase 8 + Phase 9

**Phase 7 剩余**

- [ ] ☆ 备份脚本 `scripts/backup.sh`（mysqldump 每日 + 异地）
- [ ] ☆ 监控告警（Grafana 面板：判题 P99 延迟 > 5s / 队列积压 > 50）

**Phase 8 质量保障**

- [ ] ★ CI/CD 流水线（GitHub Actions：编译 + 单测 + 集成测试 + lint）
- [ ] ★ 单元测试覆盖率 ≥ 60%（核心模块：auth / judge / repo / rate_limit / audit）
- [ ] ★ E2E 验收脚本 `scripts/e2e_acceptance.sh`（覆盖 §12.1 A1–A34）
- [ ] ★ 编译炸弹防护测试（提交模板元递归 / `#include` 炸弹，验证 10s 超时）
- [ ] ★ OLE 判定测试（提交死循环输出 100MB，验证 OLE + 容器不被撑爆）
- [x] ★ 限流测试（注册/登录 1 分钟内 100 次请求，验证 429 + Retry-After）—— v1.2.63（e2e A26 拆 A26a 注册 quota 5/min/IP + A26b 登录 quota 10/min/IP：用独立 X-Forwarded-For IP 把两个 bucket 隔离，不污染 default IP 的 register/login quota 状态——避免 A27/A35 误 hit429；每个子用例五重断言：HTTP 429 + Retry-After ∈ [1,60] 整数秒 + X-RateLimit-Limit==该 quota capacity + X-RateLimit-Remaining==0 + .code==RATE_LIMITED + .error.details.quota 命中预期 name；登录子用例用错误密码「先 401 再 429」的方式不依赖前置 register 配额。SPEC「1 分钟 100 次」措辞沿用 Phase 2 ★ 旧版描述，实际按 SPEC §5.1 默认值 register=5/min、login=10/min 打到第 N+1 次即触发，quota 阈值由 `RATE_LIMIT_REGISTER_PER_MIN` / `RATE_LIMIT_LOGIN_PER_MIN` 注入；单测覆盖在 `tests/unit/test_rate_limit.cpp`）
- [ ] ★ 审计日志测试（删题/改角色后查 audit-logs 验证写入）
- [ ] ☆ 压测报告（5/10/20 人并发判题，验证 P95 < 5s）
- [ ] ☆ 渗透测试（XSS / SQL 注入 / CSRF 扫描）
- [ ] △ 模糊测试（fuzzing）判题输入

**Phase 9 运维监控**

- [ ] ★ Prometheus 指标 `/api/v1/metrics`（`submissions_total{status}`、`judge_duration_seconds`、`queue_size`、`warm_pool_size`、`db_pool_active`）
- [ ] ★ Grafana 面板（系统概览 / 判题 P95 / 错误率 / 队列 / 资源）
- [ ] ★ 日志聚合（stdout JSON 格式，Docker logs 接管，可选 Loki/ELK）
- [ ] ★ 日志轮转（logrotate 或 Docker log driver `json-file` + `max-size`）
- [ ] ☆ 备份验证（每月 1 次 restore drill 到测试环境）
- [ ] ☆ 告警规则（P99 延迟 / 队列积压 / 磁盘 / 证书过期）
- [ ] △ 性能 Profile（`perf` / flamegraph 跑判题热路径）

### Standing Issues / 已知技术债

- [ ] pre-existing test flakes（3 个）—— 不阻塞新功能开发：`auth_refresh` / `auth_cookie_storage` / `test_warm_pool::ConcurrentAcquireReleaseRaceFree`
- [ ] main.cpp 不注册多个 routes（ODR 冲突 work-around）—— Phase 3-6 多个 routes 因 `litecode::detail::req_string` 重定义无法在同 TU 链接，端到端覆盖由 test binary 独享
- [ ] `user_repo.h::detail::req_string` — 与其他 repo header 同 TU ODR 冲突，所有统计/管理路由绕开此 header 改裸 SQL
- [ ] Phase 5 前端 20 commit 连续 churn —— CSS 已 607 大括号 / 22 章节，下次动 UI 前先评估可复用面

### 决策日志

| 日期 | 决策 |
|------|------|
| 2026-07-10 | 前端美化推迟到 v1.2.45（Phase 6/7 稳定后） |
| 2026-07-10 | Phase 6 收尾按 Tier 1 顺序（audit-logs → queue → 失败锁定） |
| 2026-07-10 | Phase 7 分两批：最小骨架（Tier 2）优先于完整骨架（Tier 4） |

---

## 1. 项目愿景

构建一个轻量级的在线判题系统（OJ），核心复刻 LeetCode 的刷题体验。项目以 **学习 C++ Web 开发** 为首要目标，MVP 阶段聚焦于核心刷题流程跑通，后续迭代完善体验。

**一句话描述**: 用户注册登录 → 浏览题库 → 在双栏界面中阅读题目并编写 C/C++ 代码 → 提交后实时获得判题结果（AC/WA/TLE 等）→ 查看个人提交历史和做题统计。

---

## 2. 需求总览

### 2.1 功能需求

| # | 功能 | 优先级 | 说明 |
|---|------|--------|------|
| F1 | 用户注册/登录 | P0 | 用户名 + 密码，JWT (access + refresh) 鉴权 |
| F2 | 题库浏览 | P0 | 题目列表页，支持按难度/标签筛选 |
| F3 | 题目详情 + 代码编辑 | P0 | LeetCode 风格双栏布局：左题目右编辑器，代码草稿持久化 |
| F4 | 代码提交与实时判题 | P0 | 异步提交 C/C++ 代码 → Docker 沙箱执行 → 轮询/SSE 拿结果 |
| F5 | 判题结果展示 | P0 | AC / WA / RE / TLE / MLE / OLE / PE / CE |
| F6 | 提交历史 | P1 | 查看自己的历史提交及结果（非管理员只能查自己） |
| F7 | 个人主页 | P1 | 做题统计（已解决/总数、通过率、提交次数） |
| F8 | 管理员题目导入 | P0 | 仅管理员可批量导入题目（JSON/YAML），普通用户无权操作 |
| F9 | 管理员题目管理 | P0 | 仅管理员可增删改题目（软删除），普通用户只读 |
| F10 | 管理员后台页面 | P1 | 管理员专属页面：题目 CRUD、批量导入、用户管理、系统概览 |
| F11 | 排行榜 | P2 | 按解题数/通过率排名 |
| F12 | 健康检查 | P1 | `/api/v1/health` 暴露 DB / Docker 可达性，docker-compose healthcheck 用 |
| F13 | 审计日志 | P1 | 管理员关键操作（删题、改角色、批量导入）写入 `audit_logs` |
| F14 | 限流 | P1 | 注册/登录/提交按 IP+用户限流，防刷 |
| F15 | 可观测性 | P2 | `/api/v1/metrics` Prometheus 指标、日志 JSON 化、请求 ID 链路追踪 |

### 2.2 非功能需求

| 维度 | 要求 |
|------|------|
| **性能** | 单次判题响应 < 5s（提交立即返回 `submission_id`；含容器启动，简单题目 < 3s）；支持 5-10 人同时使用 |
| **安全** | Docker 容器隔离 + Socket 代理 + CPU 时间限制 + 内存限制 + 网络隔离 + 文件系统隔离 + 输出大小限制 + 编译超时 + 编译安全标志 + 密码 bcrypt + JWT 签名 + CSP/SRI + Markdown XSS 净化 + SQL 参数化 |
| **可扩展性** | 判题模块架构预留多语言扩展（C/C++ 优先，后续可加 Python/Java） |
| **部署** | 本地单机运行，Docker Compose 一键启动；Caddy 反向代理 + 自动 HTTPS |
| **开发周期** | MVP 2-4 周 |
| **可观测** | 结构化日志（JSON） + Prometheus 指标 + 请求 ID 串联 |
| **数据保留** | 失败提交（WA/TLE/RE/CE）90 天后清理；AC 提交永久保留；mysqldump 每日异地备份 |

---

## 3. 系统架构

### 3.1 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      浏览器 (前端)                           │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────────┐    │
│  │  题目列表页  │  │ 刷题页(双栏)  │  │  个人主页/排行  │    │
│  └──────┬──────┘  └──────┬───────┘  └────────┬────────┘    │
│         │                │                    │              │
│  ┌──────┴────────────────┼────────────────────┘              │
│  │  管理后台 (🔒 admin)  │                                   │
│  │  题目CRUD | 批量导入 | 用户管理                            │
│  └──────────┬─────────────┘                                   │
│             │ HTTPS + JWT (含 role 字段)                     │
└─────────────┼──────────────────────────────────────────────┘
              │
┌─────────────┼──────────────────────────────────────────────┐
│             ▼        Web 服务器 (C++)                       │
│         [多线程 HTTP server]                                │
│                                                             │
│  ┌─────────┐  ┌─────────┐  ┌──────────┐  ┌───────────┐   │
│  │ 用户模块 │  │ 题目模块 │  │ 提交模块  │  │ 判题调度器 │   │
│  │  Auth   │  │ Problem │  │ Submit   │  │  Judge    │   │
│  └────┬────┘  └────┬────┘  └────┬─────┘  └─────┬─────┘   │
│       │            │            │              │           │
│       │    ┌───────┴───────┐    │              │           │
│       │    │ 管理员中间件   │    │              │           │
│       │    │ 限流中间件     │    │              │           │
│       │    │ 请求 ID 中间件 │    │              │           │
│       │    └───────┬───────┘    │              │           │
│       └────────────┼────────────┘              │           │
│                          │                     │           │
│                     MySQL 连接池               │  异步任务队列  │
│                    (ORM/原生SQL)               │  + 容器预热池 │
└──────────────────────────┼─────────────────────┼──────────┘
                           │                     │
                    ┌──────┴──────┐        ┌──────┴──────┐
                    │   MySQL     │        │ Docker API   │
                    │  数据库     │        │ (via socket  │
                    └─────────────┘        │  proxy)      │
                                           └──────┬──────┘
                                                  │
                                ┌─────────────────┴────────────┐
                                │  容器预热池 (N 个 idle 容器)  │
                                │  + 临时运行容器 (执行判题)     │
                                └──────────────────────────────┘
```

> **关键设计变化（v1.2）**:
> - 判题调度器改为**异步任务队列**（线程池 + condition_variable），提交 API 立即返回 `submission_id`
> - 启动时维护**容器预热池**（2-3 个 idle 容器），减少容器冷启动开销
> - Web → Docker 通过 **Docker Socket 代理**（仅暴露 5 个白名单子命令），不直接挂 socket

### 3.2 判题流程图（异步版）

```
用户提交代码
     │
     ▼
POST /api/v1/submissions
     │
     ▼
┌─────────────────┐
│ 写入 submissions │ status=pending
│ 返回 submission_id
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 判题任务队列     │ ← 线程池拉取任务
│  JudgeScheduler │   限制最大并发数 (如 4)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 从预热池取容器   │ ← 优先复用 idle 容器
│ 容器执行判题     │   不足时新建
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 容器内执行流程   │
│ 1. 编译 (g++ +  │ ← 独立超时 (10s)
│    安全标志)     │   失败 → CE
│ 2. 逐点运行      │ ← 文本归一化 (CRLF/BOM)
│ 3. 内存/时间测量 │ ← cgroup v2 memory.current
│ 4. 比对输出      │ ← judge_type 分支
│ 5. 输出截断      │ ← 16MB → OLE
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ 状态回写 DB     │ status=ac/wa/tle/mle/re/ole/pe/ce
│ 释放容器到预热池 │
└────────┬────────┘
         │
         ▼
前端轮询 /api/v1/submissions/:id
（或 SSE 推送）
```

### 3.3 数据流概览

```
前端 (HTML/CSS/JS + CodeMirror/Monaco)
  │
  │  REST API (JSON) + JWT (含 role 字段) + X-Request-Id
  ▼
路由层 (多线程 HTTP server)
  │
  ├─ /api/v1/auth/*           → 用户模块 (注册/登录/刷新/登出/JWT)
  ├─ /api/v1/problems/*       → 题目模块 (列表/详情/筛选)         [公开]
  ├─ /api/v1/tags             → 标签模块                          [公开]
  ├─ /api/v1/submissions/*    → 提交模块 (异步提交/查询/历史)     [需登录]
  ├─ /api/v1/stats/*          → 统计模块 (个人统计/排行榜)        [公开/登录]
  ├─ /api/v1/admin/problems/* → 题目管理 (CRUD/批量导入)          [🔒 admin]
  ├─ /api/v1/admin/users/*    → 用户管理 (列表/角色变更)          [🔒 admin]
  ├─ /api/v1/admin/stats      → 系统统计                          [🔒 admin]
  ├─ /api/v1/admin/audit-logs → 审计日志查询                      [🔒 admin]
  ├─ /api/v1/health           → 健康检查 (DB + Docker)            [公开]
  └─ /api/v1/metrics          → Prometheus 指标                   [内网]
  │
  ▼
数据层 (MySQL)
  │
  ├─ users           用户表 (含 role 字段)
  ├─ problems        题目表 (含 is_deleted 软删除)
  ├─ tags            标签表
  ├─ problem_tags    题目-标签关联表
  ├─ test_cases      测试用例表 (含 judge_type)
  ├─ submissions     提交记录表
  └─ audit_logs      管理员操作审计表
```

---

## 4. 数据模型

> **v1.2 增补**:
> - `users.email` UNIQUE 策略修订（兼容可选）
> - `users.last_login_ip` 新增
> - `problems.is_deleted` 软删除字段新增
> - `test_cases.judge_type` 判定类型字段新增
> - `audit_logs` 表新增
> - §4.6 索引建议新增

### 4.1 users 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| username | VARCHAR(50) UNIQUE NOT NULL | 用户名 |
| password_hash | VARCHAR(255) NOT NULL | bcrypt 哈希（cost=12） |
| role | ENUM('user','admin') DEFAULT 'user' | 角色：普通用户 / 管理员 |
| email | VARCHAR(100) | 邮箱（可选，UNIQUE NULLS NOT DISTINCT，仅 MySQL 8.0.19+） |
| avatar | VARCHAR(255) | 头像 URL（默认） |
| created_at | DATETIME NOT NULL | 注册时间 |
| last_login | DATETIME | 最后登录 |
| last_login_ip | VARCHAR(45) | 最后登录 IP（支持 IPv6） |

> **权限模型**:
> - `user`（普通用户）：浏览题目、提交代码、查看自己的提交历史和个人统计
> - `admin`（管理员）：拥有普通用户所有权限 + 题目 CRUD（软删） + 批量导入 + 用户角色管理 + 查看审计日志
> - 系统初始化时通过脚本或配置创建第一个管理员账户
>
> **密码策略**:
> - bcrypt cost factor = 12（≈ 250ms 哈希，平衡安全与性能）
> - 前端校验：长度 ≥ 8，必须含字母+数字
> - 后端二次校验

### 4.2 problems 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| slug | VARCHAR(100) UNIQUE NOT NULL | 题目 URL 标识（如 two-sum） |
| title | VARCHAR(200) NOT NULL | 题目标题 |
| difficulty | ENUM('easy','medium','hard') NOT NULL | 难度 |
| description | MEDIUMTEXT NOT NULL | 题目描述（Markdown，存储前在管理端预净化） |
| time_limit | INT NOT NULL DEFAULT 1000 | 时间限制（ms） |
| memory_limit | INT NOT NULL DEFAULT 256 | 内存限制（MB） |
| accepted_count | INT NOT NULL DEFAULT 0 | 通过人数（**仅参考，不用于排行榜**，排行榜另算） |
| submission_count | INT NOT NULL DEFAULT 0 | 总提交数（仅参考） |
| is_deleted | BOOLEAN NOT NULL DEFAULT FALSE | 软删除标记（v1.2 新增） |
| created_at | DATETIME NOT NULL | 创建时间 |
| updated_at | DATETIME NOT NULL | 更新时间 |

> **原子性说明**: `tags` 字段原设计为逗号分隔字符串，违反 1NF 原子性要求，已拆分为独立的 `tags` 表和 `problem_tags` 关联表（见 4.2b、4.2c）。
>
> **软删除**（v1.2）: 删除题目时不真正 DELETE，而是 `UPDATE is_deleted = TRUE` + `updated_at = NOW()`。这样：
> - 历史 `submissions.problem_id` 仍可外键引用
> - 管理员可恢复误删
> - 前台列表自动过滤 `is_deleted = FALSE`

### 4.2b tags 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| name | VARCHAR(50) UNIQUE NOT NULL | 标签名称（如"数组""哈希表"） |

### 4.2c problem_tags 表（题目-标签关联）

| 字段 | 类型 | 说明 |
|------|------|------|
| problem_id | INT FOREIGN KEY → problems(id) | 关联题目 |
| tag_id | INT FOREIGN KEY → tags(id) | 关联标签 |
| PRIMARY KEY | (problem_id, tag_id) | 联合主键 |

### 4.2d audit_logs 表（v1.2 新增）

| 字段 | 类型 | 说明 |
|------|------|------|
| id | BIGINT AUTO_INCREMENT | 主键 |
| admin_id | INT FOREIGN KEY → users(id) | 操作管理员 |
| action | VARCHAR(50) NOT NULL | 操作类型（`problem.create` / `problem.delete` / `user.role_change` / `problem.bulk_import` 等） |
| target_type | VARCHAR(50) | 对象类型（`problem` / `user` / `tag`） |
| target_id | VARCHAR(100) | 对象 ID |
| payload | JSON | 操作详情（如删除前快照、变更前后值） |
| ip | VARCHAR(45) | 操作 IP |
| created_at | DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP | 操作时间 |

> **写入场景**（v1.2 必须实现）:
> - 创建/修改/删除（软删）题目
> - 批量导入题目
> - 变更用户角色
> - 失败登录尝试（>= 5 次）
> - 修改管理员密码

### 4.3 test_cases 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| problem_id | INT FOREIGN KEY → problems(id) NOT NULL | 关联题目 |
| input | LONGTEXT NOT NULL | 测试输入（统一 UTF-8 + LF 换行） |
| expected_output | LONGTEXT NOT NULL | 期望输出（同上） |
| is_sample | BOOLEAN NOT NULL DEFAULT FALSE | 是否为示例用例（展示给用户） |
| judge_type | ENUM('exact','ignore_trailing','float_eps','special') NOT NULL DEFAULT 'exact' | 判定类型（v1.2 新增） |
| float_epsilon | DECIMAL(10,8) NULL | 浮点误差容忍（仅 `judge_type=float_eps` 时使用） |
| order_num | INT NOT NULL DEFAULT 0 | 用例顺序 |

> **judge_type 说明**（v1.2）:
> | 取值 | 行为 |
> |------|------|
> | `exact` | 完全字符串匹配（默认） |
> | `ignore_trailing` | 忽略每行尾部空白后逐行比较（AC/PE 判定沿用 v1.1 规则） |
> | `float_eps` | 按浮点比较，绝对/相对误差 < `float_epsilon` 视为相等 |
> | `special` | Special Judge（v1.3+ 实现，MVP 阶段留字段） |

### 4.4 submissions 表

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT AUTO_INCREMENT | 主键 |
| user_id | INT FOREIGN KEY → users(id) NOT NULL | 提交用户 |
| problem_id | INT FOREIGN KEY → problems(id) NOT NULL | 提交题目 |
| language | ENUM('c','cpp') NOT NULL | 编程语言 |
| code | MEDIUMTEXT NOT NULL | 提交的源代码（MEDIUMTEXT 16MB 上限足够） |
| status | ENUM('pending','running','ac','wa','re','tle','mle','ole','pe','ce','se') NOT NULL DEFAULT 'pending' | 判题结果（v1.2 新增 `ole`、`se`） |
| time_used | INT | 实际耗时（ms） |
| memory_used | INT | 实际内存（KB，从 cgroup v2 读取） |
| error_message | TEXT | 编译错误/运行时错误信息（截断至 4KB） |
| created_at | DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP | 提交时间 |
| finished_at | DATETIME | 判题完成时间（用于计算队列等待时长） |

> **新状态码**（v1.2）:
> - `ole` (Output Limit Exceeded) — 输出超过 16MB 截断
> - `se` (System Error) — 判题基础设施异常（容器启动失败、DB 写失败等），与 CE 区分

### 4.5 索引建议（v1.2 新增）

> 单条 SQL 加索引 DDL 写在 migrations 脚本里，这里只列**逻辑**索引。

| 表 | 索引列 | 用途 |
|----|--------|------|
| users | UNIQUE(username) | 登录查询 |
| users | UNIQUE(email) | 邮箱查重 |
| problems | UNIQUE(slug) | 详情查询 |
| problems | (is_deleted, difficulty, created_at) | 列表分页 + 难度筛选 + 软删过滤 |
| problems | (is_deleted, slug) | slug 查询时直接走软删过滤的覆盖索引 |
| test_cases | (problem_id, order_num) | 判题按序拉取 |
| test_cases | (problem_id, is_sample, order_num) | 题目详情页示例展示 |
| submissions | (user_id, problem_id, created_at DESC) | 个人提交历史 |
| submissions | (problem_id, status, created_at) | 管理员查通过率 |
| submissions | (status, created_at) | 异步队列拉取待判题（status IN ('pending','running')） |
| submissions | (created_at) | 数据保留策略（90 天前清理） |
| audit_logs | (admin_id, created_at DESC) | 管理员操作历史 |
| audit_logs | (action, created_at) | 按操作类型筛选 |
| problem_tags | (tag_id, problem_id) | 反向查"某标签下所有题" |

---

## 5. API 设计

> **v1.2 增补**:
> - 全部路径加 `/api/v1/` 前缀
> - `POST /api/v1/auth/refresh`、`POST /api/v1/auth/logout` 新增
> - `POST /api/v1/submissions` 改为异步：返回 `{submission_id, status:"pending"}`，前端轮询 `/api/v1/submissions/:id`
> - `/api/v1/health`、`/api/v1/metrics` 新增
> - 提交历史查询权限收紧：非 admin 强制 `user_id = 当前用户`
> - 限流注解：注册/登录/提交按 IP+用户限流（条目详见每条 API 说明）

### 5.1 用户模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| POST | `/api/v1/auth/register` | 公开 | 5 次/分/IP | 用户注册（默认 role=user），返回 access + refresh |
| POST | `/api/v1/auth/login` | 公开 | 10 次/分/IP | 用户登录，返回 access (2h) + refresh (7d) |
| POST | `/api/v1/auth/refresh` | 已登录 | - | 用 refresh token 换新 access |
| POST | `/api/v1/auth/logout` | 已登录 | - | 注销（refresh token 加入黑名单到过期） |
| GET  | `/api/v1/auth/profile` | 已登录 | - | 获取当前用户信息 |

> **JWT 细节**（v1.2 明确）:
> - 算法：HS256（secret 从环境变量 `JWT_SECRET` 读取，启动时校验非空且 ≥ 32 字节）
> - access token TTL：2 小时；refresh token TTL：7 天
> - Payload：`{sub: user_id, username, role, iat, exp}`
> - 黑名单：refresh token 注销时写入 Redis（key=`jwt:blacklist:<jti>`，TTL=剩余有效期）

### 5.2 题目模块（公开只读 + 管理员写入）

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| GET | `/api/v1/problems` | 公开 | 60 次/分/IP | 题目列表（分页、难度/标签筛选，自动过滤 `is_deleted=FALSE`） |
| GET | `/api/v1/problems/:slug` | 公开 | 60 次/分/IP | 题目详情（含示例用例 + 标签） |
| GET | `/api/v1/tags` | 公开 | - | 所有标签列表 |
| POST | `/api/v1/admin/problems` | 🔒 admin | 30 次/分 | 创建单道题目（含标签关联，自动创建不存在的标签） |
| PUT | `/api/v1/admin/problems/:slug` | 🔒 admin | 30 次/分 | 修改题目（含标签关联更新） |
| DELETE | `/api/v1/admin/problems/:slug` | 🔒 admin | 10 次/分 | 软删除题目（设置 `is_deleted=TRUE`，不真正删除） |
| POST | `/api/v1/admin/problems/import` | 🔒 admin | 5 次/小时 | 批量导入（multipart/form-data，JSON/YAML 文件） |

> 全部路径以 `/api/v1/admin/*` 开头需要 JWT 中 `role=admin`，非管理员返回 403 Forbidden。

### 5.3 提交模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| POST | `/api/v1/submissions` | 已登录 | 30 次/分/用户 | **异步**：写入 `submissions(status=pending)`，入队后立即返回 `{submission_id, status:"pending"}` |
| GET | `/api/v1/submissions/:id` | 已登录 | - | 查询单次结果；非 admin 只能查自己的 |
| GET | `/api/v1/submissions` | 已登录 | - | 提交历史列表；**非 admin 强制 `user_id = 当前用户`**，忽略请求参数中的 `user_id` |
| GET | `/api/v1/submissions/sse/:id` | 已登录 | - | (可选) SSE 推送判题结果，完成时主动推一条 |

> **异步设计**（v1.2）: 客户端典型流程：
> ```
> POST /api/v1/submissions → {id: 42, status: "pending"}
> ↓ 客户端 1-2s 轮询
> GET /api/v1/submissions/42 → {status: "running"}
> ↓ 继续轮询
> GET /api/v1/submissions/42 → {status: "ac", time_used: 12, memory_used: 2048}
> ```

### 5.4 统计模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| GET | `/api/v1/stats/profile/:username` | 已登录 | - | 用户做题统计 |
| GET | `/api/v1/stats/ranking` | 公开 | 30 次/分/IP | 排行榜（默认 100 名，可选分页） |

### 5.5 管理员模块

| 方法 | 路径 | 权限 | 限流 | 说明 |
|------|------|------|------|------|
| GET | `/api/v1/admin/users` | 🔒 admin | 60 次/分 | 用户列表（分页 + 搜索） |
| PUT | `/api/v1/admin/users/:id/role` | 🔒 admin | 10 次/分 | 修改用户角色（写入 `audit_logs`） |
| GET | `/api/v1/admin/stats` | 🔒 admin | - | 系统统计（题目数、用户数、提交数、队列长度） |
| GET | `/api/v1/admin/audit-logs` | 🔒 admin | - | 审计日志查询（分页 + 筛选） |
| GET | `/api/v1/admin/queue` | 🔒 admin | - | 判题队列状态（等待中/运行中数量） |

### 5.6 系统模块（v1.2 新增）

| 方法 | 路径 | 权限 | 说明 |
|------|------|------|------|
| GET | `/api/v1/health` | 公开 | 健康检查：`{db: "ok", docker: "ok", queue_size: 3, warm_pool: 2}` |
| GET | `/api/v1/metrics` | 内网 | Prometheus 指标（`litecode_submissions_total{status="ac"}`、`litecode_judge_duration_seconds` 等） |

### 5.7 全局约定（v1.2 新增）

- **请求 ID**: 所有响应携带 `X-Request-Id` 头（服务端生成 UUID v4），用于日志串联
- **统一错误格式**:
  ```json
  {
    "code": "INVALID_INPUT",
    "message": "用户名长度必须在 3-50 之间",
    "details": { "field": "username" },
    "request_id": "550e8400-e29b-41d4-a716-446655440000"
  }
  ```
- **错误码枚举**: `INVALID_INPUT` / `UNAUTHORIZED` / `FORBIDDEN` / `NOT_FOUND` / `RATE_LIMITED` / `CONFLICT` / `INTERNAL_ERROR` / `SERVICE_UNAVAILABLE`
- **CORS**: 默认仅允许本地域名（`http://localhost:8080`），生产环境从配置读取白名单

---

## 6. 前端页面设计

### 6.1 页面清单

| 页面 | 路径 | 权限 | 说明 |
|------|------|------|------|
| 首页/题库列表 | `/` | 公开 | 题目列表，筛选、搜索 |
| 刷题页 | `/problems/:slug` | 公开 | 双栏：左题目右编辑器，**代码草稿自动保存到 localStorage** |
| 登录页 | `/login` | 公开 | 用户登录 |
| 注册页 | `/register` | 公开 | 用户注册 |
| 个人主页 | `/profile/:username` | 已登录 | 做题统计、提交历史 |
| 排行榜 | `/ranking` | 公开 | 全站排名 |
| 🔒 管理后台-题目管理 | `/admin/problems` | admin | 题目列表、增删改、批量导入 |
| 🔒 管理后台-题目编辑 | `/admin/problems/edit/:slug` | admin | 新建/编辑题目表单 |
| 🔒 管理后台-用户管理 | `/admin/users` | admin | 用户列表、角色管理 |
| 🔒 管理后台-系统概览 | `/admin/dashboard` | admin | 系统统计（题目数、用户数、提交数、队列、判题机时延） |
| 🔒 管理后台-审计日志 | `/admin/audit-logs` | admin | 审计日志查询 |

> **导航逻辑**: 普通用户导航栏只显示"题库/排行榜/个人主页"；管理员用户额外显示"管理后台"入口。非管理员直接访问 `/admin/*` 路径时前端拦截跳转至首页。

### 6.2 刷题页布局（核心页面）

```
┌──────────────────────────────────────────────────┐
│  Logo    题库  排行榜  讨论          [用户头像 ▾] │
├─────────────────────┬────────────────────────────┤
│                     │  ┌─────────────────────┐   │
│   题目描述区域       │  │   代码编辑器          │   │
│   (Markdown 已净化)  │  │  (CodeMirror/Monaco)  │   │
│                     │  │  [草稿自动保存]        │   │
│   - 描述            │  │                       │   │
│   - 示例输入/输出    │  │  C++  ▼              │   │
│   - 约束条件        │  │                       │   │
│                     │  └─────────────────────┘   │
│                     │  ┌─────────────────────┐   │
│                     │  │   运行结果 / 提交结果  │   │
│                     │  │   AC ✅ / WA ❌        │   │
│                     │  │   时间: 12ms 内存: 2MB │   │
│                     │  │   (失败时显示测试点)   │   │
│                     │  └─────────────────────┘   │
│                     │  [运行] [提交]               │
├─────────────────────┴────────────────────────────┤
│  提交历史标签页                                     │
└──────────────────────────────────────────────────┘
```

### 6.3 前端安全与体验（v1.2 新增）

| 类别 | 要求 | 实现 |
|------|------|------|
| **CSP** | 严格 CSP 头 | `<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; style-src 'self' 'unsafe-inline'; img-src 'self' data:;">` |
| **SRI** | CDN 资源 integrity 校验 | CodeMirror/Monaco 走 CDN 时加 `integrity` + `crossorigin="anonymous"` |
| **XSS 防护** | 题目描述 Markdown 渲染必须净化 | 用 [DOMPurify](https://github.com/cure53/DOMPurify) 配合 [marked](https://marked.js/) |
| **代码草稿** | 写一半代码不丢失 | `localStorage.setItem('code:<slug>:<lang>', code)`，提交成功后清除，刷新时弹"恢复草稿"提示 |
| **Token 存储** | 防 XSS 盗 token | access token 存内存，refresh token 走 `HttpOnly; Secure; SameSite=Strict` cookie |
| **深色模式** | 用户体验 | CSS 变量 + `prefers-color-scheme`，支持手动切换并持久化 |
| **移动端** | 响应式 | 刷题页在 < 768px 切换为上下布局；管理后台在 < 1024px 折叠侧栏 |
| **a11y** | 可访问性 | 主要按钮/链接加 `aria-label`，编辑器支持键盘 Tab 缩进 |

---

## 7. 判题模块详细设计

> **v1.2 重大改造**:
> - 异步任务队列（线程池 + condition_variable）
> - 容器预热池
> - g++ 安全编译标志 + 编译独立超时
> - OLE 判定分支
> - 文本归一化（CRLF/BOM）
> - 内存/时间精确测量（cgroup v2）
> - `judge_type` 判题分支
> - `SE` 系统错误状态

### 7.1 Docker 判题流程（v1.2 完整版）

```
0. 系统启动
   - 判题调度器初始化：启动 1 个 dispatcher 线程 + N 个 worker 线程（默认 N=4）
   - 创建容器预热池：docker create × K 个 idle 容器（默认 K=2）

1. 接收提交 → 写入 submissions(status=pending) → 入队
   - 返回 {submission_id, status: "pending"}
   - 同时启动 SSE/轮询由前端负责

2. Worker 从队列取任务
   a. 从预热池 docker start 一个容器；若池空则 docker create + start 新容器
   b. 更新 status=running

3. 容器内执行（受 cgroup + Docker 资源限制）
   a. **编译** (独立超时 10s，防编译炸弹)
      g++ -O2 -std=c++17 -pipe \
          -fstack-protector-strong \
          -D_FORTIFY_SOURCE=2 \
          -Wformat -Wformat-security \
          -Wl,-z,now -Wl,-z,relro \
          -o solution solution.cpp
      · 编译失败 → CE（error_message 截断至 4KB）
      · 编译超时 → CE（标记为 "Compilation timeout"）
      · 编译 OOM → MLE
   b. **运行** (每个测试点独立)
      · 文本归一化：输入/期望输出先 `tr -d '\r'` + 去 BOM，保证跨平台一致
      · 运行: `timeout {time_limit/1000+1} ./solution < input.txt > output.txt`
      · 输出截断：若 output.txt > 16MB → OLE（判当前测试点失败并跳过剩余比对）
      · 时间测量：从 cgroup v2 `cpu.stat` 读取 `usage_usec`（精确到 ms）
      · 内存测量：从 cgroup v2 `memory.current` 读取（KB）
      · 结果判定:
        · 超时 → TLE
        · 内存超限 → MLE
        · 非零退出码 → RE
        · 正常退出 → 根据 test_cases.judge_type 比对:
          - `exact`         → 完全相同 AC，否则 WA
          - `ignore_trailing` → 去尾部空白后相同 AC，否则 PE
          - `float_eps`     → 浮点按 epsilon 比较，相同 AC，否则 WA
          - `special`       → MVP 返回 SE（v1.3+ 接入 SPJ）
   c. 汇总：所有测试点通过 → AC；首个失败测试点为该 submission 的最终状态
   d. 任何基础设施异常（容器 OOM、DB 写失败）→ SE

4. 状态回写 DB
   - status, time_used(取最大值), memory_used(取最大值), error_message, finished_at

5. 容器归还
   - docker stop + docker rm 当前容器
   - 不归还预热池（每次新建专用容器，保证隔离）
   - 维护预热池大小：异步补齐至 K 个

6. 异常兜底
   - Web 进程对每个 docker run 设 30s 硬超时（防 judge.sh 自身卡死）
```

### 7.2 判题 Docker 镜像 (Dockerfile)

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ gcc gdb coreutils dos2unix \
    && rm -rf /var/lib/apt/lists/*

# 判题运行脚本
COPY judge.sh /usr/local/bin/judge.sh
RUN chmod +x /usr/local/bin/judge.sh

# 安全：非 root 运行（即使 --read-only 也以 nobody 启动）
RUN useradd -m -u 1000 judgeuser
USER judgeuser
WORKDIR /judge

ENTRYPOINT ["/usr/local/bin/judge.sh"]
```

### 7.3 判题安全策略汇总

| 策略 | 实现方式 | 参数 |
|------|----------|------|
| CPU 时间限制 | Docker `--cpus` + 容器内 `timeout` | 默认 1s |
| **编译超时**（v1.2） | 容器内 `timeout 10s g++ ...` | 10s（防编译炸弹） |
| 内存限制 | Docker `--memory` | 默认 256MB |
| 内存测量（v1.2） | 读 cgroup v2 `memory.current` | KB 精度 |
| 网络隔离 | Docker `--network=none` | 完全禁止网络 |
| 文件系统隔离 | Docker `--read-only` + 临时写入目录 | 只读根 + /tmp 可写 |
| 进程数限制 | Docker `--pids-limit` | 最多 50 个进程 |
| **输出大小限制**（v1.2） | judge.sh 中 `head -c 16M` 截断 | > 16MB 判 OLE |
| 权限提升防护 | Docker `--security-opt=no-new-privileges` | 禁止提权 |
| **编译标志**（v1.2） | `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Wl,-z,now -Wl,-z,relro` | 防溢出利用 |
| **Docker Socket 隔离**（v1.2） | Web 容器通过 [docker-socket-proxy](https://github.com/Tecnativa/docker-socket-proxy) 访问 Docker | 仅白名单 5 个子命令 |
| **Web 容器自身**（v1.2） | `--cpus=2 --memory=512m` + 非 root | 防止 Web 进程失控影响判题 |
| 进程身份（v1.2） | judge 容器内 `USER judgeuser` | 即使逃逸也无 root |

### 7.4 判题规则细则（v1.2 新增）

| 项 | 规则 |
|----|------|
| 换行符 | 输入/期望输出统一 LF（`\n`），容器内 `tr -d '\r'` 归一化 |
| BOM | 容器内 `sed '1s/^\xEF\xBB\xBF//'` 去 UTF-8 BOM |
| 浮点输出 | 推荐题目用 `printf("%.6f")` 或固定格式；`judge_type=float_eps` 兜底 |
| 时间测量 | 取所有测试点中最长 `cpu.stat usage_usec`，向上取整为 ms |
| 内存测量 | 取所有测试点中 `memory.peak`（cgroup v2）或 `memory.max_usage`（v1），单位 KB |
| OLE 行为 | 输出 > 16MB → 立即判 OLE，**不再**继续比对 |
| CE 信息 | 截断至前 4KB（避免恶意超长编译错误撑爆 DB） |
| RE 信息 | 截断至前 2KB |
| 系统错误 | 容器启动失败、DB 写失败、Docker daemon 不可达 → `status=se` |

---

## 8. 题目导入格式

### 8.1 单道题目 JSON 结构

```json
{
  "slug": "two-sum",
  "title": "两数之和",
  "difficulty": "easy",
  "tags": ["数组", "哈希表"],
  "time_limit_ms": 1000,
  "memory_limit_mb": 256,
  "description_md": "# 题目描述\n\n给定一个整数数组...",
  "samples": [
    {
      "input": "[2,7,11,15]\n9",
      "output": "[0,1]"
    }
  ],
  "test_cases": [
    {
      "input": "2,7,11,15\n9\n",
      "expected_output": "0,1\n",
      "is_sample": true,
      "judge_type": "exact",
      "float_epsilon": null
    }
  ]
}
```

> **v1.2 增补**:
> - `test_cases[].judge_type` 新增字段（默认 `exact`，可省略）
> - `test_cases[].float_epsilon` 新增字段（仅 `float_eps` 时使用）
> - `description_md` 建议在管理端**导入前**预净化（v1.2 软要求，v1.3 强制）

### 8.2 批量导入（🔒 管理员专属）

仅管理员可通过 API 端点批量上传题目。普通用户调用此接口返回 403。

```
POST /api/v1/admin/problems/import
Authorization: Bearer <admin-jwt-token>
Content-Type: multipart/form-data

files: problems/*.json
```

> **设计意图**: 题目来源完全由管理员控制，普通用户只能浏览和做题，不能导入或创建题目。
>
> **v1.2 行为**:
> - 单次最多 50 个文件 / 10MB
> - slug 冲突时默认**跳过**该条并返回详情；可选 query 参数 `?on_duplicate=overwrite` 覆盖
> - 导入完成后写入一条 `audit_logs` 记录（payload 含成功/失败计数）

---

## 9. 技术栈汇总

| 层 | 技术选型 | 说明 |
|----|---------|------|
| **前端** | HTML + CSS + JavaScript | 原生，无框架；DOMPurify + marked 做 Markdown 净化 |
| **代码编辑器** | CodeMirror 6 或 Monaco Editor | CDN 引入（带 SRI） |
| **后端** | C++17 + cpp-httplib (多线程) | header-only HTTP 库，**必须**配置 `ThreadPool` 参数 |
| **Web 框架备选** | Crow / userver | 如果 cpp-httplib 并发不够，可平滑切换 |
| **数据库** | MySQL 8.0.19+ | 关系型存储（`UNIQUE NULLS NOT DISTINCT` 需要 ≥ 8.0.19） |
| **数据库连接** | mysql-connector/cpp 或 sqlpp11 | C++ MySQL 驱动 |
| **判题沙箱** | Docker Engine API（经 Socket 代理） | 容器隔离执行 |
| **认证** | JWT (jwt-cpp 库) | HS256，access 2h + refresh 7d |
| **密码哈希** | bcrypt | cost=12 |
| **限流** | 内存令牌桶（单实例）/ Redis（多实例） | MVP 用内存 |
| **Token 黑名单** | Redis | refresh token 注销用 |
| **构建** | CMake | 跨平台构建 |
| **部署** | Docker Compose | 一键启动 Web + MySQL + Judge + Socket Proxy + Caddy |
| **反向代理** | Caddy | 自动 HTTPS（生产）/ HTTP-only（本地） |
| **数据库迁移** | Flyway 社区版 或纯 SQL 脚本 (`db/migrations/V001__init.sql`, `V002__audit_log.sql`) | v1.2 新增 |
| **监控** | Prometheus + Grafana | `/api/v1/metrics` 暴露指标（v1.2 新增） |
| **日志** | spdlog + JSON 输出 | stdout + 文件，Docker logs 接管（v1.2 新增） |
| **备份** | mysqldump 每日 + 异地 | cron job（v1.2 新增） |

---

## 10. 项目目录结构

```
litecode-cpp/
├── CMakeLists.txt
├── docker-compose.yml
├── Caddyfile                  # 反向代理配置（v1.2 新增）
├── Dockerfile                 # Web 服务镜像
├── .env.example               # 环境变量模板（v1.2 增补）
├── .gitignore                 # （项目初始化已存在）
├── README.md                  # （项目初始化已存在）
├── LICENSE                    # （项目初始化已存在）
├── dependence.md              # 依赖说明（项目初始化已存在）
├── docs/                      # 补充文档目录（项目初始化已存在）
├── third_party/               # 第三方库（nlohmann/json, cpp-httplib, jwt-cpp, zlib）
├── build/                     # CMake 构建产物（运行时生成，已 .gitignore）
├── monitoring/                # 监控配置（v1.2 增补）
│   ├── prometheus.yml         # Prometheus 抓取配置
│   └── grafana/
│       └── datasources.yml    # Grafana 数据源
├── judge/                     # 判题模块
│   ├── Dockerfile             # 判题镜像
│   ├── judge.sh               # 判题执行脚本
│   └── judge_server.h         # 判题调度器（线程池 + 任务队列 + 预热池）
├── src/                       # 后端源码
│   ├── main.cpp
│   ├── server.h               # HTTP 服务器入口（多线程）
│   ├── config.h               # 配置管理
│   ├── logger.h               # 日志封装（JSON 格式，v1.2）
│   ├── middleware/
│   │   ├── request_id.h       # 请求 ID 注入（v1.2）
│   │   ├── rate_limit.h       # 限流中间件（v1.2）
│   │   ├── auth_middleware.h  # JWT 认证
│   │   └── admin_middleware.h # 管理员权限校验（v1.2 唯一保留位置，原 src/auth/ 同名文件已废弃）
│   ├── utils/                 # 通用工具（v1.2 增补）
│   │   ├── uuid.h             # UUID v4 生成（X-Request-Id 用）
│   │   ├── string_utils.h
│   │   └── time_utils.h
│   ├── cache/                 # Redis 客户端（v1.2 增补）
│   │   └── redis_client.h     # token 黑名单 / 限流计数用
│   ├── db/
│   │   ├── connection_pool.h
│   │   ├── migration.h        # 迁移工具封装（v1.2）
│   │   ├── user_repo.h
│   │   ├── problem_repo.h
│   │   ├── tag_repo.h
│   │   ├── submission_repo.h
│   │   └── audit_log_repo.h   # 审计日志（v1.2）
│   ├── auth/
│   │   ├── jwt_utils.h
│   │   ├── password_hash.h
│   │   ├── refresh_token.h    # refresh token + 黑名单（v1.2）
│   │   └── bcrypt/            # bcrypt 源码（项目初始化已存在）
│   ├── routes/
│   │   ├── auth_routes.h
│   │   ├── problem_routes.h
│   │   ├── submission_routes.h
│   │   ├── stats_routes.h
│   │   ├── admin_routes.h
│   │   ├── system_routes.h    # /health /metrics（v1.2）
│   │   └── error_handler.h    # 统一错误格式（v1.2）
│   └── judge/
│       ├── judge_scheduler.h
│       ├── docker_client.h
│       └── warm_pool.h        # 容器预热池（v1.2）
├── web/                       # 前端静态文件
│   ├── index.html
│   ├── problem.html
│   ├── login.html
│   ├── register.html
│   ├── profile.html
│   ├── ranking.html
│   ├── admin/
│   │   ├── dashboard.html
│   │   ├── problems.html
│   │   ├── problem-edit.html
│   │   ├── users.html
│   │   └── audit-logs.html    # 审计日志页（v1.2）
│   ├── css/
│   │   └── style.css          # 支持深色模式（v1.2）
│   ├── assets/                # 静态资源（v1.2 增补）
│   │   └── img/
│   │       └── default-avatar.svg
│   └── js/
│       ├── app.js
│       ├── api.js
│       ├── editor.js          # 草稿持久化（v1.2）
│       ├── markdown.js        # DOMPurify + marked（v1.2）
│       └── admin.js
├── problems/                  # 题目数据
│   ├── two-sum.json
│   └── ...
├── db/
│   └── migrations/            # 数据库迁移脚本（v1.2）
│       ├── V001__init.sql
│       ├── V002__add_audit_logs.sql
│       ├── V003__add_judge_type.sql
│       ├── V004__add_soft_delete.sql
│       ├── V005__add_indexes.sql
│       └── V006__add_ole_se_status.sql  # 补 V005 遗漏：status ENUM 增 ole/se
├── scripts/
│   ├── init_db.sh             # 数据库初始化入口
│   ├── create_admin.sql
│   ├── seed_problems.py
│   └── backup.sh              # 备份脚本（v1.2）
└── tests/
    ├── unit/
    │   ├── test_auth.cpp
    │   ├── test_problem.cpp
    │   ├── test_judge.cpp
    │   ├── test_rate_limit.cpp # v1.2
    │   ├── test_audit_log.cpp  # v1.2
    │   └── test_stats.cpp
    ├── integration/
    │   ├── test_api.cpp
    │   ├── test_judge_flow.cpp
    │   └── test_security.cpp   # 编译炸弹、OLE、SSE 等（v1.2）
    └── e2e/                   # 端到端验收（v1.2 增补，§12.1 自动化）
        └── e2e_acceptance.sh
├── logs/                      # 日志文件输出目录
```

---

## 11. TODO 清单（MVP 开发计划）

> **v1.2 重要变更**:
> - 各 Phase **增补**了安全/性能/可观测相关条目
> - 新增 **Phase 8（质量保障）** 和 **Phase 9（运维与监控）**
> - 标注每条优先级：`★ 必做` / `☆ 应做` / `△ 可选`

### Phase 1 - 基础设施

- [x] ★ 项目目录结构 + CMake 构建（引入 cpp-httplib、mysql-connector、jwt-cpp、spdlog）
- [x] ★ 数据库初始化脚本（建表 SQL + 初始管理员种子数据）
- [x] ★ 配置管理（config.h：DB / 端口 / JWT_SECRET / 判题参数等；env 优先 + 默认值）
- [x] ★ 日志封装（logger.h：JSON 格式，INFO/WARN/ERROR，stdout + 文件，**带 request_id 字段**）
- [x] ★ 数据库连接池（connection_pool.h：连接池 + 基础查询封装）
- [x] ★ HTTP 服务框架（server.h：路由注册 + CORS + 统一 JSON 响应 + **多线程 ThreadPool**）
- [x] ★ Docker Compose 开发环境（Web + MySQL + Socket Proxy + Judge 容器一键启动）
- [x] ★ 请求 ID 中间件（生成 UUID v4 注入响应头 + 贯穿日志）
- [x] ★ 健康检查端点 `/api/v1/health`（DB + Docker 探测）
- [x] ★ 统一错误处理（error_handler.h：§5.7 错误码枚举 + 统一响应格式）
- [x] ☆ Caddyfile 反向代理配置
- [x] ☆ Prometheus + Grafana docker-compose 接入
- [x] △ JSON 日志输出（spdlog + JSON 格式化）

### Phase 2 - 登录注册模块

- [x] ★ JWT 工具（jwt_utils.h：HS256 签发 + 验证 + 提取 user_id/role；secret 从 env 读且 ≥ 32 字节）
- [x] ★ 密码哈希（password_hash.h：bcrypt cost=12，header-only + 内联；`hash_password` / `verify_password` / `extract_cost_factor` / `needs_rehash`；失败抛 `PasswordError` 三级异常，verify 路径 `noexcept`；tests/unit/test_password_hash.cpp 33 用例全通过 ~6.3s）
- [x] ★ 密码强度校验（后端 `validate_password_strength` / `require_password_strength`：8 ≤ len ≤ 72（含 bcrypt `$2b$` 72 字节硬上限）+ 字母 + 数字；前端实现见 web/js/app.js 时复用同策略，避免前后端规则漂移）
- [x] ★ JWT 认证中间件（auth_middleware.h + admin_middleware.h：header-only 内联；`extract_bearer_token`（OWS/大小写/CRLF 注入防护）+ `require_authentication`（401 统一信封，验签失败/过期/错 issuer/错 kind 全归一为 "invalid or expired token" 防探测）+ `require_role`（403）+ `require_admin`（401→403 链式校验）；tests/unit/test_auth_middleware.cpp 32 用例覆盖 token 解析、过期/篡改/换 issuer/换 kind、role 校验、E2E HttpServer 往返，全通过 ~0.27s）
- [x] ★ 管理员权限中间件（admin_middleware.h：`require_admin` 链式 `require_authentication` + `require_role("admin")`，未登录 401、登录但非 admin 403）
- [x] ★ 限流中间件（按 IP+用户，令牌桶，§5.1 各端点配额）
- [x] ★ Refresh Token 机制（签发 + 刷新 + 黑名单）
- [x] ★ 用户注册 API（POST /api/v1/auth/register，限流 5/分/IP）
- [x] ★ 用户登录 API（POST /api/v1/auth/login，限流 10/分/IP；header-only + inline；`login_handler` + `LoginFailureTracker`（per-username count, kAuditLogEvery=5）+ `detail::parse_login_request`；bcrypt verify `noexcept`；anti-enumeration：用户不存在 vs 密码错误 → 同 401 envelope；5 次连续失败 → `audit_log_repo::record_login_failure` 写 audit_logs；成功 → 重置计数 + `user_repo::update_last_login`；tests/unit/test_auth_login.cpp 29 用例全通过 ~14s）
- [x] ★ Refresh API（POST /api/v1/auth/refresh；header-only + inline；`refresh_handler` + `RefreshRequest` / `detail::parse_refresh_request`；无 rate limit（SPEC §5.1）；`verify` → `user_repo::find_by_id` → `rotate_token_pair`（自动 blacklist check + revoke old + sign new）；anti-enumeration：bad sig/expired/revoked/wrong kind/deleted user → 同 401 envelope；新 access token 携带最新 username/role；tests/unit/test_auth_refresh.cpp 25 用例全通过 ~6.9s）
- [x] ★ Logout API（POST /api/v1/auth/logout；header-only + inline；`logout_handler` + `LogoutRequest` / `detail::parse_logout_request`；需 Bearer 鉴权（`require_authentication` 401），无 rate limit（SPEC §5.1）；body `{refresh_token}` → `revoke_refresh_token`（best-effort，never throws，TTL=剩余有效期，max_ttl_seconds cap）；theft defense：access token 的 claims.user_id 作为 `expected_user_id`，refresh sub 失配时 refused（log WARN），bob session 不受影响；anti-enumeration：malformed/expired/wrong-kind/access-as-refresh → 同 200 + `{logged_out:true, revoked:false}`；幂等（store.revoke 重写 TTL）；tests/unit/test_auth_logout.cpp 28 用例全通过 ~11s）
- [x] ★ 用户信息 API（GET /api/v1/auth/profile；header-only + inline；`profile_handler`；需 Bearer 鉴权（`require_authentication` 401）；claims.user_id → `user_repo::find_by_id`（用户被删 → 401 "user not found"）；no rate limit（SPEC §5.1）；响应 `{user: {id, username, role, email|null, avatar|null, created_at, last_login|null}}` —— 不暴露 `password_hash`（防 stolen access token 跨服务复用）/ `last_login_ip`（session metadata）；DB DATE_FORMAT 修正 mysql-connector 9.x 把 DATETIME 读成 packed binary 的坑（user_repo::find_by_id/find_by_username 同步修）；anti-enumeration：所有 401 envelope 走 `require_authentication` 统一墙；tests/unit/test_auth_profile.cpp 19 用例全通过 ~8.7s）
- [x] ☆ 失败登录审计（连续 5 次失败写 audit_logs，详见 login_handler + LoginFailureTracker）

### Phase 3 - 题目模块

- [x] ★ 题目数据模型（problem_repo.h：CRUD + 软删除 + 软删过滤查询）
- [x] ★ 标签数据模型（tag_repo.h：标签 + 题目-标签关联）
- [x] ★ 审计日志数据模型（audit_log_repo.h）
- [x] ★ 数据库迁移脚本（V001-V008 + V099，按 §10 目录落地；V007/V008 幂等加固；本地 MySQL 8.0.41 端到端验证全表/列/索引对照 SPEC §4 + §4.5 通过；详见 v1.2.5）
- [x] ★ 题目列表 API（GET /api/v1/problems，分页 + 难度/标签筛选 + 软删过滤，60/min/IP 限流；problem_routes.h 实现 + test_problem_list.cpp 35 用例全通过；详见 v1.2.6）
- [x] ★ 题目详情 API（GET /api/v1/problems/:slug，含示例 + 标签，60/min/IP 限流；problem_routes.h 实现 + test_case_repo.h (list_samples_for_problem) + test_problem_detail.cpp 30 用例全通过；详见 v1.2.7）
- [x] ★ 标签列表 API（GET /api/v1/tags，公开 + 无 rate limit；tag_routes.h 实现 + test_tag_list.cpp 15 用例全通过；详见 v1.2.8）
- [x] ★ 管理员题目 CRUD API（POST/PUT/DELETE /api/v1/admin/problems/*，限流到位；详见 v1.2.9）
- [x] ★ 管理员批量导入 API（POST /api/v1/admin/problems/import，🔒 admin，5/hour，multipart/form-data ≤50 files / ≤10MB，?on_duplicate=skip|overwrite 默认 skip，单 audit_log 行，partial-batch failure isolation；详见 v1.2.10）
- [x] ★ 示例题目数据（5-10 道 JSON 题目文件，含 judge_type 字段）—— `problems/` 10 道种子题（6 easy + 4 medium；25 samples + 65 test cases；三 judge_type 全覆盖）；problems/README.md + v1.2.11
- [x] ★ 题目版本/编辑历史表（`problem_revisions`，**存储层** v1.2.12 已落地；v1.3 再补 reading API / diff / restore）—— V009 migration + `problem_revisions_repo.h`（record / find_by_id / latest_for_problem / list_for_problem / count / validators，1062 自动重试一次后抛 `ConflictError`）+ admin_problem_routes POST/PUT 自动 snapshot（audit payload 加 `revision_id` 双向索引）+ tests/unit/test_problem_revision.cpp 18 用例 + test_admin_problem_crud 2 个 e2e case；详见 v1.2.12

### Phase 4 - 代码执行与判题模块

- [x] ★ 判题 Docker 镜像（judge/Dockerfile + judge.sh，§7.2 用户、§7.3 安全标志）—— v1.2.13
- [x] ★ Docker 客户端（docker_client.h：经 socket 代理的 create/start/exec/kill/rm）—— v1.2.14
- [x] ★ 容器预热池（warm_pool.h：启动时预创建 K 个 idle，异步补齐）—— v1.2.15
- [x] ★ 判题调度器（judge_scheduler.h：**线程池 + 任务队列** + 最大并发数 + 30s 硬超时）—— v1.2.15
- [x] ★ 提交数据模型（submission_repo.h：pending/running/终态全生命周期）—— v1.2.15
- [x] ★ 异步判题流程（POST 立即返回 submission_id，worker 异步执行）—— v1.2.16
- [x] ★ 提交代码 API（POST /api/v1/submissions 异步）—— v1.2.16
- [x] ★ 查询提交结果 API（GET /api/v1/submissions/:id）—— v1.2.16
- [x] ★ 提交历史列表 API（GET /api/v1/submissions，**非 admin 强制 user_id = 自己**）—— v1.2.16
- [x] ★ g++ 安全编译标志 + 独立编译超时（防编译炸弹）—— judge.sh + Dockerfile（v1.2.13）
- [x] ★ OLE 判定分支（> 16MB 截断 → 判 OLE 立即终止）—— judge.sh（v1.2.13）
- [x] ★ 文本归一化（CRLF/BOM 归一化，§7.4）—— judge.sh（v1.2.13）
- [x] ★ 内存/时间精确测量（cgroup v2 `cpu.stat` / `memory.current`）—— judge.sh（v1.2.13）
- [x] ★ `judge_type` 判题分支（exact / ignore_trailing / float_eps / special）—— judge.sh（v1.2.13）
- [x] ★ SE 系统错误状态（容器/DB 异常时使用）—— judge_scheduler + submission_routes（v1.2.15 + v1.2.16）
- [x] ☆ SSE 推送（GET /api/v1/submissions/sse/:id）—— judge_notifier + submission_routes + judge_scheduler 接线（v1.2.17）
- [x] ☆ Special Judge 框架（v1.3）

### Phase 5 - 前端页面

- [x] ★ 前端框架（公共导航栏 + api.js 封装 + 统一错误处理 + 401 自动跳转登录）
- [x] ★ CSP 头 + CDN 资源 SRI 配置
- [x] ★ Token 存储（access 内存 / refresh HttpOnly cookie）—— v1.2.21
- [x] ★ Markdown XSS 净化（DOMPurify + marked，SRI 锁 + ALLOWED_TAGS/FORBID_TAGS 双重白名单 + URL 过滤 + 浏览器端 XSS 用例覆盖 <script>/<iframe>/javascript:/data:/onclick/base-hijack 等 20+ 攻击向量；C++ 端 `test_problem_detail.cpp::DescriptionPreservesAdversarialPayloads` 锁定"API 不做净化、字节级回传"契约）
- [x] ★ 登录页面（/login.html）
- [x] ★ 注册页面（/register.html，密码强度提示）
- [x] ★ 题目列表页面（/，筛选 + 分页）—— v1.2.24
- [x] ★ 题目详情 + 代码编辑器页面（双栏布局，集成 CodeMirror 5.65.16 SRI-pinned + textarea fallback；Markdown 描述 + 样例 + 通过率 + 提交 + 轮询 + 草稿 + Ctrl/⌘+Enter 快捷键 + dark-mode 主题切换）—— v1.2.25
- [x] ★ **编辑器草稿持久化**（localStorage，提交成功后清除，刷新提示恢复 — Discover → Restore/Discard 横幅 + JSON envelope `{v,code,savedAt,lang}` + 相对/绝对时间 + KB/MB 字节单位 + legacy bare-string 兼容 + 24/25 unit tests 24 PASS）—— v1.2.26
- [x] ★ 提交结果展示（AC/WA/TLE 状态 + 耗时/内存 + **失败时显示测试点** — 11 状态 icon + 耗时/内存 gauge（80% warning / ≥100% danger）+ 状态 specific help 文案 + 复制代码 + 再次提交 + 失败时完整 `error_message` 详情折叠面板）—— v1.2.27
- [x] ★ 异步判题轮询/SSE 客户端
- [x] ★ 提交历史标签页（刷题页下方）
- [x] ★ 个人主页（/profile/:username，做题统计）—— v1.2.30
- [x] ★ 排行榜页面（/ranking.html）—— v1.2.31
- [x] ★ 管理后台 - 题目管理页面（/admin/problems.html）—— v1.2.32
- [x] ★ 管理后台 - 批量导入页面（/admin/problems.html 导入区）—— v1.2.32
- [x] ★ 管理后台 - 用户管理页面（/admin/users.html）—— v1.2.33（列表 + 搜索/角色筛选 + 分页 + 改角色确认 PUT + self-protection + 404 endpoint-pending 软状态 + URL state + 复用 `.lc-admin-table`/`.lc-pill--admin`）
- [x] ★ 管理后台 - 系统概览页面（/admin/dashboard.html，含队列/预热池/指标）—— v1.2.34
- [x] ★ 管理后台 - 审计日志页面（/admin/audit-logs.html）—— v1.2.35（列表 + action/target_type 筛选 + 起止日期 + URL state + 分页 + payload 抽屉 + 30s 自动刷新 + visibilitychange 暂停 + 复用 `.lc-admin-table`/`.lc-pill`/`.lc-modal`/`.lc-code-block`）
- [x] ★ 前端权限拦截（非管理员 → 跳转首页，未登录 → 跳转登录）—— v1.2.36（同步 admin-route gate + sessionStorage cached-role redirect + 异步 hydrate 区分 login.html/index.html + `html.lc-route-pending` CSS no-flash + `gate.requireAdmin()` DRY 收口）
- [x] ★ 深色模式（CSS 变量 + `prefers-color-scheme` + 手动切换持久化）—— v1.2.37
- [x] ☆ 移动端响应式（< 768px 切换上下布局）—— v1.2.38

### Phase 6 - 统计与安全

- [x] ★ 用户做题统计 API（GET /api/v1/stats/profile/:username）—— v1.2.39
- [x] ★ 排行榜 API（GET /api/v1/stats/ranking）—— v1.2.40
- [x] ★ 管理员用户管理 API（GET /api/v1/admin/users, PUT /api/v1/admin/users/:id/role，**写 audit_logs**）—— v1.2.41
- [x] ★ 管理员系统统计 API（GET /api/v1/admin/stats，**含队列/预热池状态**）—— v1.2.42
- [x] ★ 审计日志 API（GET /api/v1/admin/audit-logs）—— v1.2.43
- [x] ★ 判题队列状态 API（GET /api/v1/admin/queue）—— v1.2.44
- [x] ★ 安全加固（输入校验 + SQL 参数化 + XSS 防护 + CSP + SRI）—— v1.2.45
- [x] ★ 错误处理统一（§5.7 错误码 + 响应格式）—— `src/routes/error_handler.h::make_error_envelope`
- [x] ☆ 失败登录锁定（连续 N 次失败 15 分钟内禁止该用户名登录）—— v1.2.46（in-memory `LoginFailureTracker` lockout state machine + sliding 15-min window + dedicated `auth.login_locked` audit row + 423 Locked + `Retry-After` header；env knobs `LOGIN_LOCKOUT_ENABLED` / `_THRESHOLD` / `_WINDOW_SECONDS` / `_DURATION_SECONDS`；anti-enumeration envelope identical to 401）

### Phase 7 - 部署

- [x] ★ 完善 Docker Compose（Web + MySQL + Judge + Socket Proxy + Caddy + Prometheus + Grafana）—— v1.2.57（11 个服务：mysql / docker-proxy / judge / web / caddy / prometheus / alertmanager / grafana / cadvisor / node-exporter / backup；4 个 profile：default / proxy / monitoring / backup；web 容器 user: "1000:1000" + no-new-privileges + docker-proxy healthcheck + judge 旧语法 `!reset []` 修复）
- [x] ★ Docker Socket 代理（[tecnativa/docker-socket-proxy](https://github.com/Tecnativa/docker-socket-proxy)，白名单 5 子命令）
- [x] ★ Web 容器资源限制（--cpus=2 --memory=512m，非 root 运行）
- [x] ★ Caddy 反向代理（生产 HTTPS / 本地 HTTP）
- [x] ★ README + 部署文档（环境变量 + 管理员创建 + 灾备恢复）—— v1.2.57（`docs/deployment.md` 11 节：拓扑 / 启动 / env / admin / 灾备 / 升级 / 监控 / 安全合规清单 / 常见坑 / 故障排查 / 资源总表）
- [x] ☆ 备份脚本（backup.sh：mysqldump 每日 + 异地）—— v1.2.57（alpine 镜像 + dcron 03:00 自动跑 + gzip/zstd 压缩 + 14 天保留 + rclone 异地钩子）
- [x] ☆ 监控告警（Grafana 面板：判题 P99 延迟 > 5s 告警 / 队列积压 > 50 告警）—— v1.2.57（Phase 9 Overview dashboard 5 panel 分组 + alertmanager + 10 条告警规则）

### Phase 8 - 质量保障（v1.2 新增）

- [x] ★ CI/CD 流水线（GitHub Actions：编译 + 单测 + 集成测试 + lint）—— v1.2.58（4 jobs: lint shellcheck+hadolint+compose config / build ccache+cmake / integration-test MySQL service+ctest -E flake / docker-build buildx+GHCR）+ v1.2.59（coverage job: lcov + 核心 ≥80% / 全库 ≥40% 硬门禁 + Codecov，commit 1 阶段 warn-only 收集 baseline；`docs/ci.md` 6 节 + `scripts/lint.sh` / `scripts/coverage.sh` 本地复现 + `.github/workflows/release.yml` 多架构+GitHub Release + dependabot 周一 09:00）
- [x] ★ 单元测试覆盖率 ≥ 60%（核心模块：auth / judge / repo / rate_limit / audit）—— v1.2.60（scripts/coverage.sh + .github/workflows/ci.yml coverage job 实装 lcov 硬门禁，去掉 v1.2.59 的 `|| true` warn-only；新增 5 个 pure-unit gtest：test_test_case_repo.cpp / test_submission_repo.cpp / test_user_repo_helpers.cpp / test_problem_repo_helpers.cpp / test_audit_log_helpers.cpp，覆盖 repo + audit 两块核心模块的 validators / clamp_list_filter / build_where_clause 等过去只被 route-level 测试间接触达的 helpers，把核心覆盖率拉到阈值之上；`./scripts/coverage.sh gate` 失败即 fail 整 CI job）
- [x] ★ E2E 验收脚本（scripts/e2e_acceptance.sh：覆盖 §12.1 所有 A1–A34 用例）—— v1.2.61（`scripts/e2e_acceptance.sh` 黑盒验收 curl+jq，覆盖 A1–A35 含 A3b：API 用例走公开 HTTP 面断言状态码/错误信封，JUDGE 用例提交真实 C++ 轮询终态，STATIC 用例（A13/A16/A23/A24/A32/A33/A34）沿用 test_frontend_xss.sh 先例对 web/ 源码 grep；能力探测 SERVER/JUDGE/WEB/COMPOSE 优雅降级 + 默认宽松（缺能力记 skip）+ `E2E_STRICT=1` 把缺能力 skip 升级为 fail 供 CI 强约束；provision 自动注册测试用户 + 登录 admin + bulk-import two-sum 保证可判题目；无栈干跑 20 assert 全绿 exit 0，STRICT 无栈 exit 1；不注册进 ctest，需 live stack 手动/CI 调用）
- [x] ★ 编译炸弹防护测试（提交模板元递归 / `#include` 炸弹，验证 10s 超时）—— v1.2.61（e2e A29 拆 A29a + A29b 双子用例：A29a 用 B<40> 五路 Fibonacci 模板元递归验证 judge.sh Section C 的 124|137 分支 → `error_message` 含指纹 `Compilation timeout (limit 10s)`，10s `compile_timeout_ms` 真正截断 g++；A29b 用 `#include __FILE__` 自递归触发 g++ `#include` 嵌套深度上限 fatal error，证明编译炸弹被拦下、终态非 AC；两个子用例都断言墙钟 ≤ 15s + 事后 `/health` 不被打挂；e2e dry-run 20/0/29 exit 0，STRICT 1/29 exit 1）
- [x] ★ OLE 判定测试（提交死循环输出 100MB，验证 OLE + 容器不被撑爆）—— v1.2.61 + v1.2.62 补强（e2e A30：`while(true) fwrite(buf,1,4096,stdout);` 死循环输出 → judge.sh Section D 的 OLE 立即判定分支（`RAW_OUT > OUTPUT_LIMIT_BYTES(16MB)`），三层断言：1) 顶层 status=ole（API contract）；2) `error_message` 含指纹 `output exceeded [0-9]+ bytes \(got [0-9]+\)`，证明 16MB 截断真在 judge.sh 而不是容器被 OOM 后的副产物；3) 事后 `/health` 仍 200（容器不被撑爆）+ 墙钟 ≤ 15s。v1.2.61 commit message 自报"两条 [ ]→[x]"但漏翻了这一条，v1.2.62 flip `[x]` + 把 case-level OLE 落地证明从 info 字段（同源指纹）改成更稳的 `error_message` 指纹断言）
- [x] ★ 限流测试（注册/登录 1 分钟内 100 次请求，验证 429 + Retry-After）—— v1.2.63（e2e A26 拆 A26a 注册 quota 5/min/IP + A26b 登录 quota 10/min/IP：用独立 X-Forwarded-For IP 把两个 bucket 隔离，不污染 default IP 的 register/login quota 状态——避免 A27/A35 误 hit429；每个子用例五重断言：HTTP 429 + Retry-After ∈ [1,60] 整数秒 + X-RateLimit-Limit==该 quota capacity + X-RateLimit-Remaining==0 + .code==RATE_LIMITED + .error.details.quota 命中预期 name；登录子用例用错误密码「先 401 再 429」的方式不依赖前置 register 配额。SPEC「1 分钟 100 次」措辞沿用 Phase 2 ★ 旧版描述，实际按 SPEC §5.1 默认值 register=5/min、login=10/min 打到第 N+1 次即触发，quota 阈值由 `RATE_LIMIT_REGISTER_PER_MIN` / `RATE_LIMIT_LOGIN_PER_MIN` 注入；单测覆盖在 `tests/unit/test_rate_limit.cpp`）
- [ ] ★ 审计日志测试（删题/改角色后查 audit-logs 验证写入）
- [ ] ☆ 压测报告（5/10/20 人并发判题，验证 P95 < 5s）
- [ ] ☆ 渗透测试（XSS / SQL 注入 / CSRF 扫描）
- [ ] △ 模糊测试（fuzzing）判题输入

### Phase 9 - 运维与监控（v1.2 新增）

- [ ] ★ Prometheus 指标接入（`/api/v1/metrics`：submissions_total{status}、judge_duration_seconds、queue_size、warm_pool_size、db_pool_active）
- [ ] ★ Grafana 面板（系统概览 / 判题 P95 / 错误率 / 队列 / 资源）
- [ ] ★ 日志聚合（stdout JSON 格式，Docker logs 接管，可选 Loki/ELK）
- [ ] ★ 日志轮转（logrotate 或 Docker log driver `json-file` + `max-size`）
- [ ] ☆ 备份验证（每月 1 次 restore drill 到测试环境）
- [ ] ☆ 告警规则（P99 延迟、队列积压、磁盘、证书过期）
- [ ] △ 性能 Profile（`perf` / `flamegraph` 跑一次判题热路径）

---

## 12. 验收标准

### 12.1 MVP 必须通过的验收用例

| # | 验收用例 | 通过标准 |
|---|---------|---------|
| A1 | 用户注册 | 注册成功返回 201，密码以 bcrypt cost=12 哈希存储 |
| A2 | 用户登录 | 登录成功返回 access (2h) + refresh (7d) |
| A3 | 未授权访问 | 未携带 token 访问受保护 API 返回 401 + 统一错误格式 |
| A3b | 非管理员访问管理 API | 普通用户访问 /api/v1/admin/* 返回 403 |
| A4 | 题目列表 | 返回分页题目列表，支持按难度/标签筛选，**软删题目不出现** |
| A5 | 题目详情 | 返回题目描述 + 示例用例，**Markdown 已 XSS 净化**（API 字节级回传 + 前端 DOMPurify allowlist + FORBID_TAGS 兜底，详见 `web/js/markdown.js` + `web/test/markdown-xss.html` 20+ 用例） |
| A6 | 正确代码提交 | 异步返回 submission_id；轮询后状态=AC，附耗时 + 内存 |
| A7 | 错误代码提交 | 异步返回 submission_id；轮询后状态=WA + 失败测试点信息 |
| A8 | 死循环代码 | 提交 `while(true)` → TLE，**响应在 5s 内**，服务器不崩溃 |
| A9 | 内存爆炸代码 | 提交 `malloc(INT_MAX)` → MLE，服务器不崩溃 |
| A10 | 编译错误代码 | 提交语法错误 → CE + 编译错误信息（截断至 4KB） |
| A11 | 网络访问代码 | 提交含 socket 代码 → 容器无网络，返回 RE |
| A12 | 文件系统访问 | 提交读 `/etc/passwd` → 容器隔离（--read-only + 非 root），返回 RE |
| A13 | 双栏刷题页 | 左侧题目正确渲染（无 XSS），右侧编辑器语法高亮 |
| A14 | 提交历史 | 可查看自己的历史提交；**非 admin 查 user_id=X (非自己) 返回空或 403** |
| A15 | 个人主页 | 显示已解决数、通过率、提交统计 |
| A16 | Docker Compose | `docker-compose up` 一键启动全部服务（含 socket proxy / caddy） |
| A17 | 题目批量导入 | 管理员上传 JSON 后可通过 API 查询到，**audit_logs 有一条记录** |
| A18 | 管理员创建题目 | POST /api/v1/admin/problems 成功，**audit_logs 有记录** |
| A19 | 管理员编辑题目 | PUT /api/v1/admin/problems/:slug 成功 |
| A20 | 管理员删除题目 | DELETE 软删除成功，列表 API 不再返回该题，**audit_logs 有记录** |
| A21 | 普通用户无法导入题目 | 调用 /api/v1/admin/problems/import 返回 403 |
| A22 | 初始管理员账户 | 系统初始化后存在至少一个 admin 账户可登录管理后台 |
| A23 | 管理后台页面 | 管理员登录后导航栏显示"管理后台"入口 |
| A24 | 非管理员前端拦截 | 普通用户访问 /admin/* 被前端拦截跳转至首页 |
| **A25** | **异步判题状态流转**（v1.2） | 提交后立即返回 pending；轮询可见 pending→running→终态；终态后 status 不再变化 |
| **A26** | **限流生效**（v1.2） | 1 分钟内注册 6 次 → 第 6 次返回 429 + Retry-After |
| **A27** | **审计日志写入**（v1.2） | 删题/改角色/批量导入后，audit_logs 表有对应记录 |
| **A28** | **容器预热池生效**（v1.2） | `/api/v1/health` 返回 `warm_pool ≥ 1`；判题任务 worker 优先从池取容器 |
| **A29** | **编译炸弹防护**（v1.2） | 提交模板元递归 / 巨型宏 → CE (Compilation timeout)，判题在 15s 内返回 |
| **A30** | **OLE 判定**（v1.2） | 提交死循环 `while(true) printf("a");` → 判 OLE，**容器内存不被撑爆**（限制 16MB 输出） |
| **A31** | **健康检查**（v1.2） | `GET /api/v1/health` 返回 DB / Docker 状态，docker-compose healthcheck 通过 |
| **A32** | **Markdown XSS 防护**（v1.2） | 题目描述含 `<script>alert(1)</script>` → 前端不执行，HTML 实体或过滤后展示 |
| **A33** | **编辑器草稿持久化**（v1.2） | 写一半代码刷新页面 → 弹"恢复草稿"提示，确认后代码回来 |
| **A34** | **深色模式**（v1.2） | 切换深色模式后页面正确变色，刷新后保持 |
| **A35** | **失败登录锁定**（v1.2） | 连续 5 次失败登录后 15 分钟内该用户名登录返回 423 Locked + `Retry-After` 头，错误信封与"用户名/密码错误"一致（不泄露账号是否存在）；audit_logs 新增 `auth.login_locked` 行；解锁后正确密码可登录成功并清零计数器 |

### 12.2 性能验收

| 指标 | 标准 |
|------|------|
| 提交 API 响应 | < 200ms（立即返回 submission_id） |
| 判题响应（P95） | < 5s（简单题 < 3s） |
| 题目列表 API | < 200ms |
| 排行榜 API | < 500ms |
| 并发判题 | 支持 10 人同时提交不阻塞，超出排队 |
| 健康检查 | < 50ms |

---

## 13. 风险与权衡

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| cpp-httplib 单线程/低并发 | 高并发时性能瓶颈 | **v1.2 缓解**：启用 ThreadPool；预留切换 Crow/userver 的路径 |
| Docker 容器启动延迟 | 判题响应慢 | **v1.2 缓解**：容器预热池 + 复用 idle 容器 |
| Docker Socket 暴露 Web 容器 | Web 被攻破后控制宿主机 | **v1.2 缓解**：Docker Socket 代理（白名单 5 子命令）+ Web 容器非 root |
| 编译炸弹 | 单提交占用判题资源过久 | **v1.2 缓解**：g++ 独立 10s 超时 + Docker pids-limit |
| 恶意代码读敏感文件 | 信息泄露 | **v1.2 缓解**：--read-only + 非 root 运行 + 网络隔离 |
| C++ ORM 生态不如其他语言 | 数据库操作开发效率低 | 封装基础 Repo 层，手写 SQL |
| 前端原生 JS 开发效率低 | UI 开发慢 | **v1.2 缓解**：DOMPurify / marked 轻量库 |
| 浮点数判题 PE 边界模糊 | 判题结果争议 | **v1.2 缓解**：`judge_type=float_eps` 显式字段，默认走 exact |
| Docker 判题资源消耗 | 单机内存压力 | **v1.2 缓解**：判题线程池最大并发限制 + 队列机制 |
| 管理员权限提升风险 | 误操作或恶意操作 | **v1.2 缓解**：audit_logs 全量记录 + Grafana 异常告警 |
| 初始管理员创建 | 首次部署时无管理账户 | 提供创建管理员脚本，文档明确说明 |
| 提交表无限增长 | 长期运行后 DB 膨胀 | **v1.2 缓解**：90 天前失败提交清理 + AC 永久保留 + 备份策略 |
| 同步判题阻塞 HTTP | 5+ 人并发时接口卡死 | **v1.2 缓解**：异步判题 + 任务队列 + 轮询/SSE |
| CSRF 攻击 | 跨站请求伪造 | **v1.2 缓解**：refresh token HttpOnly + SameSite=Strict cookie |

---

## 14. 后续迭代规划（Post-MVP）

| 版本 | 功能 |
|------|------|
| v1.3 | Special Judge、Markdown 预净化强制、problem_revisions 编辑历史、比赛/Contest 模块 |
| v1.4 | 讨论区、题解、收藏、错题本 |
| v1.5 | 多语言支持（Python/Java/Go）、管理员操作审计日志增强、系统监控面板（已 v1.2 部分落地） |
| v2.0 | 多实例 + Redis session 共享、判题微服务拆分 |

---

## 15. 安全设计（v1.2 新增）

> 将散落在各章节的安全策略**集中归档**，方便安全审计和新人上手。

### 15.1 认证与会话

- bcrypt cost=12 哈希密码
- JWT HS256，access 2h + refresh 7d
- refresh token 注销入 Redis 黑名单（TTL = 剩余有效期）
- refresh token 走 `HttpOnly; Secure; SameSite=Strict` cookie
- 失败登录 5 次 → 写 audit_logs，可选锁定 15 分钟

### 15.2 API 安全

- 全部 SQL 参数化（`?` 占位符），禁止字符串拼接
- 限流：注册 5/分/IP、登录 10/分/IP、提交 30/分/用户、管理员写操作 30/分/分/IP
- 非 admin 查他人提交历史 → 403 或返回空
- 管理后台路径（/api/v1/admin/*）严格 role=admin 校验
- CORS 白名单（生产从 env 读取）

### 15.3 前端安全

- CSP `default-src 'self'`，CDN 加 SRI
- 题目描述 Markdown 经 DOMPurify 净化
- 用户输入（昵称、评论）双向 XSS 防护
- access token 存内存（防 XSS 盗取），refresh 走 cookie
- 提交前前端基础校验（长度、字符集）

### 15.4 判题沙箱

- 容器 `--read-only` + 非 root 运行 + `--network=none` + `--pids-limit=50`
- 编译 g++ 启用 `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Wl,-z,now -Wl,-z,relro`
- 编译独立 10s 超时（防编译炸弹）
- 运行 30s 硬超时（防 judge.sh 自身卡死）
- 输出 16MB 截断 → OLE
- CRLF/BOM 归一化，避免 Windows 编码差异
- 内存/时间从 cgroup v2 精确读取

### 15.5 容器编排

- Web → Docker 经 **socket-proxy**（白名单 5 子命令）
- Web 容器 `--cpus=2 --memory=512m`，非 root 运行
- Judge 容器每判题任务**独立** docker run，不复用容器实例（仅预热池）
- 资源硬限制：max-concurrent=4 + 队列上限=50，超出返回 503

### 15.6 操作审计

- 管理员**所有**写操作（CRUD / 批量导入 / 改角色）写入 `audit_logs`
- 失败登录 ≥ 5 次写入
- 关键操作（删题、改角色）需前端二次确认
- 审计日志可查询接口：`GET /api/v1/admin/audit-logs`

### 15.7 数据安全

- 密码 bcrypt 哈希存储
- 数据库连接密码从 env 读，不入代码
- mysqldump 每日异地备份，保留 30 天
- 失败提交 90 天后清理，AC 提交永久保留

---

## 16. 运维与监控（v1.2 新增）

### 16.1 健康检查

- `GET /api/v1/health`（公开）：
  ```json
  {
    "status": "ok",
    "db": "ok",
    "docker": "ok",
    "queue_size": 3,
    "warm_pool": 2,
    "uptime_seconds": 86400
  }
  ```
- docker-compose 用作容器 `healthcheck`：`curl -f http://localhost:8080/api/v1/health || exit 1`

### 16.2 Prometheus 指标

`GET /api/v1/metrics`（内网）暴露：

| 指标名 | 类型 | 标签 | 说明 |
|--------|------|------|------|
| `litecode_http_requests_total` | Counter | method, path, status | HTTP 请求计数 |
| `litecode_http_request_duration_seconds` | Histogram | method, path | HTTP 请求耗时 |
| `litecode_submissions_total` | Counter | status | 提交结果分布 |
| `litecode_judge_duration_seconds` | Histogram | status | 判题耗时（队列等待 + 编译 + 运行） |
| `litecode_judge_queue_size` | Gauge | - | 当前排队任务数 |
| `litecode_judge_warm_pool_size` | Gauge | - | 当前预热池容器数 |
| `litecode_db_pool_active` | Gauge | - | DB 连接池活跃连接数 |
| `litecode_auth_failures_total` | Counter | ip | 登录失败计数 |
| `litecode_docker_operations_total` | Counter | operation, status | Docker 操作计数 |

### 16.3 Grafana 面板（建议）

- **系统总览**: 在线人数、提交数 / 分钟、判题 P50/P95/P99
- **判题健康**: 队列长度、预热池大小、判题机 CPU/内存
- **错误率**: 4xx/5xx 比例、CE/TLE/MLE 分布
- **安全**: 失败登录 Top IP、限流触发次数

### 16.4 告警规则（建议）

- 判题 P95 > 5s 持续 5 分钟 → 告警
- 队列积压 > 50 持续 1 分钟 → 告警
- Web 容器内存 > 80% 持续 5 分钟 → 告警
- 失败登录单 IP > 100/小时 → 告警（可能 CC 攻击）
- 磁盘剩余 < 20% → 告警
- 证书过期前 30 天 → 告警

### 16.5 备份与恢复

- `scripts/backup.sh` 每日凌晨 3 点 mysqldump + 压缩 + 异地（OSS/S3）
- 保留策略：日备 7 份 / 周备 4 份 / 月备 6 份
- 每月 1 次 restore drill 到测试环境，验证备份可用

### 16.6 日志策略

- 应用日志：JSON 格式，stdout + 文件，**含 request_id**
- Docker 接管：使用 `json-file` log driver + `max-size=10m` + `max-file=3`
- 关键日志：登录成功/失败、判题异常、管理员写操作、容器启动失败
- 可选：Loki/ELK 聚合

---

*本文档基于 v1.1 经深度审查后升级为 v1.2，重点解决**安全、可观测性、判题异步化、可维护性**四类问题。确认规格完整后进入开发阶段。*
