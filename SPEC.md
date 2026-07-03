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
- [ ] ★ 注册页面（/register.html，密码强度提示）
- [ ] ★ 题目列表页面（/，筛选 + 分页）
- [ ] ★ 题目详情 + 代码编辑器页面（双栏布局，集成 CodeMirror/Monaco）
- [ ] ★ **编辑器草稿持久化**（localStorage，提交成功后清除，刷新提示恢复）
- [ ] ★ 提交结果展示（AC/WA/TLE 状态 + 耗时/内存 + **失败时显示测试点**）
- [ ] ★ 异步判题轮询/SSE 客户端
- [ ] ★ 提交历史标签页（刷题页下方）
- [ ] ★ 个人主页（/profile/:username，做题统计）
- [ ] ★ 排行榜页面（/ranking.html）
- [ ] ★ 管理后台 - 题目管理页面（/admin/problems.html）
- [ ] ★ 管理后台 - 批量导入页面（/admin/problems.html 导入区）
- [ ] ★ 管理后台 - 用户管理页面（/admin/users.html）
- [ ] ★ 管理后台 - 系统概览页面（/admin/dashboard.html，含队列/预热池/指标）
- [ ] ★ 管理后台 - 审计日志页面（/admin/audit-logs.html）
- [ ] ★ 前端权限拦截（非管理员 → 跳转首页，未登录 → 跳转登录）
- [ ] ★ 深色模式（CSS 变量 + `prefers-color-scheme` + 手动切换持久化）
- [ ] ☆ 移动端响应式（< 768px 切换上下布局）

### Phase 6 - 统计与安全

- [ ] ★ 用户做题统计 API（GET /api/v1/stats/profile/:username）
- [ ] ★ 排行榜 API（GET /api/v1/stats/ranking）
- [ ] ★ 管理员用户管理 API（GET /api/v1/admin/users, PUT /api/v1/admin/users/:id/role，**写 audit_logs**）
- [ ] ★ 管理员系统统计 API（GET /api/v1/admin/stats，**含队列/预热池状态**）
- [ ] ★ 审计日志 API（GET /api/v1/admin/audit-logs）
- [ ] ★ 判题队列状态 API（GET /api/v1/admin/queue）
- [ ] ★ 安全加固（输入校验 + SQL 参数化 + XSS 防护 + CSP + SRI）
- [ ] ★ 错误处理统一（§5.7 错误码 + 响应格式）
- [ ] ☆ 失败登录锁定（连续 N 次失败 15 分钟内禁止该用户名登录）

### Phase 7 - 部署

- [ ] ★ 完善 Docker Compose（Web + MySQL + Judge + Socket Proxy + Caddy + Prometheus + Grafana）
- [ ] ★ Docker Socket 代理（[tecnativa/docker-socket-proxy](https://github.com/Tecnativa/docker-socket-proxy)，白名单 5 子命令）
- [ ] ★ Web 容器资源限制（--cpus=2 --memory=512m，非 root 运行）
- [ ] ★ Caddy 反向代理（生产 HTTPS / 本地 HTTP）
- [ ] ★ README + 部署文档（环境变量 + 管理员创建 + 灾备恢复）
- [ ] ☆ 备份脚本（backup.sh：mysqldump 每日 + 异地）
- [ ] ☆ 监控告警（Grafana 面板：判题 P99 延迟 > 5s 告警 / 队列积压 > 50 告警）

### Phase 8 - 质量保障（v1.2 新增）

- [ ] ★ CI/CD 流水线（GitHub Actions：编译 + 单测 + 集成测试 + lint）
- [ ] ★ 单元测试覆盖率 ≥ 60%（核心模块：auth / judge / repo / rate_limit / audit）
- [ ] ★ E2E 验收脚本（scripts/e2e_acceptance.sh：覆盖 §12.1 所有 A1–A34 用例）
- [ ] ★ 编译炸弹防护测试（提交模板元递归 / `#include` 炸弹，验证 10s 超时）
- [ ] ★ OLE 判定测试（提交死循环输出 100MB，验证 OLE + 容器不被撑爆）
- [ ] ★ 限流测试（注册/登录 1 分钟内 100 次请求，验证 429 + Retry-After）
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
