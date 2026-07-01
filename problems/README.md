# 示例题目数据 (problems/)

> 10 道种子题目，覆盖三种难度（easy/medium）与三种判题类型（exact / ignore_trailing / float_eps），
> 符合 [SPEC §8.1](../SPEC.md) 的单题 JSON 结构，并已被
> `src/routes/admin_bulk_import_routes.h`（v1.2.10）解析路径接纳。

---

## 题目清单

| # | slug | 难度 | 主要标签 | judge_type |
|---|------|------|----------|------------|
| 1 | [`two-sum.json`](./two-sum.json) | easy | 数组、哈希表 | exact |
| 2 | [`reverse-integer.json`](./reverse-integer.json) | easy | 数学 | exact |
| 3 | [`palindrome-number.json`](./palindrome-number.json) | easy | 数学 | exact |
| 4 | [`fizzbuzz.json`](./fizzbuzz.json) | easy | 数学、字符串、模拟 | exact |
| 5 | [`valid-parentheses.json`](./valid-parentheses.json) | easy | 字符串、栈 | exact |
| 6 | [`climbing-stairs.json`](./climbing-stairs.json) | easy | 动态规划、数学 | exact |
| 7 | [`longest-substring-without-repeating.json`](./longest-substring-without-repeating.json) | medium | 字符串、哈希表、滑动窗口 | exact |
| 8 | [`add-two-numbers.json`](./add-two-numbers.json) | medium | 链表、数学 | exact |
| 9 | [`sqrt-x.json`](./sqrt-x.json) | medium | 数学、二分查找 | **float_eps** |
| 10 | [`trim-trailing-whitespace.json`](./trim-trailing-whitespace.json) | medium | 字符串、双指针 | **ignore_trailing** |

合计：6 easy + 4 medium，25 个样例，65 个判题用例，每题都做了"Pure solution verification"。

---

## 单题文件格式

参照 [SPEC §8.1](../SPEC.md)，并按 `admin_bulk_import_routes.h::detail::process_one_file`
实际接受的字段（注意 `description`，不是 `description_md`——SPEC §8.1 的示例字段名与
v1.2.10 代码落地的小差异）：

```jsonc
{
  "slug": "two-sum",                  // 必填：URL 标识，匹配 ^[a-z0-9]+(-[a-z0-9]+)*$
  "title": "两数之和",                  // 必填：≤ 200 字符
  "difficulty": "easy",                // 必填：easy | medium | hard
  "description": "## 题目描述\n\n…", // 必填：Markdown（前端用 DOMPurify + marked 渲染）
  "tags": ["数组", "哈希表"],           // 可选，缺省 []；每项 1..50 字符，被 trim 后入库
  "time_limit_ms": 1000,              // 可选，缺省 1000；范围 [1, 60000]
  "memory_limit_mb": 256,             // 可选，缺省 256；范围 [1, 1024]
  "samples": [                        // 可选，缺省 []；展示给前端的样例（is_sample=TRUE 入库）
    { "input": "2 7 11 15\n9\n", "output": "0 1\n" }
  ],
  "test_cases": [                     // 可选，缺省 []；判题用例（is_sample=FALSE 入库）
    {
      "input": "2 7 11 15\n9\n",
      "expected_output": "0 1\n",
      "judge_type": "exact"           // 可选缺省 exact；exact | ignore_trailing | float_eps | special
    }
  ]
}
```

### `judge_type` 说明（v1.2 新增）

| 取值 | 行为 |
|------|------|
| `exact` | 完全字符串匹配（默认） |
| `ignore_trailing` | 忽略每行尾部空白后逐行比较（**AC/PE 规则沿用 v1.1**） |
| `float_eps` | 按浮点比较，绝对/相对误差 < `float_epsilon` 视为相等 |
| `special` | Special Judge（v1.3+ 接入，当前会让判题返回 SE） |

> 注：v1.2.10 实现的 `parse_test_cases_array` 在 wire 协议上接受 `float_epsilon`
> 字段，但实现层固定绑 `1e-8` 默认值（详见 v1.2.10 changelog + header 注释）。
> 本仓库内 `sqrt-x.json` 在 `float_eps` 用例中给出 `1e-4` 容差——其作用是说明题面期望，
> 实际入库值仍为 DB 默认值，未来 §8.1 schema 升级后再做 binding 透传。

---

## 导入到 DB 的两种方式

### 方式 A：管理员批量导入 API（推荐）

先用管理员账号登录拿到 JWT，再把若干文件作为 multipart 上传：

```bash
# 1. 登录（默认管理员：参见 scripts/create_admin.sql）
TOKEN=$(curl -s -X POST http://localhost:8080/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"admin123!"}' | jq -r .access_token)

# 2. 批量导入（默认 on_duplicate=skip；可选 overwrite）
curl -X POST "http://localhost:8080/api/v1/admin/problems/import" \
  -H "Authorization: Bearer $TOKEN" \
  -F "files=@two-sum.json" \
  -F "files=@reverse-integer.json" \
  -F "files=@add-two-numbers.json" \
  ...
```

成功后写一条 `audit_logs`（`action=problem.bulk_import`，payload 含导入统计）；详见
[v1.2.10 changelog](../SPEC.md) 与 `tests/unit/test_admin_bulk_import.cpp`。

### 方式 B：单题 CRUD（与方式 A 等价）

```bash
curl -X POST http://localhost:8080/api/v1/admin/problems \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d @two-sum.json
```

> 单题 API 同样接受以上 schema，只是 `samples` / `test_cases` 字段直接传 JSON 数组。
> 详见 `src/routes/admin_problem_routes.h` + v1.2.9 changelog。

---

## 校验

所有题目均经过两道校验：

1. **Schema 校验**（与 `admin_bulk_import_routes.h::detail::process_one_file` 一致）：
   slug 形状、必需字段、`difficulty` ∈ {easy,medium,hard}、`judge_type` ∈
   {exact,ignore_trailing,float_eps,special}、`time_limit_ms` ∈ [1, 60000]、
   `memory_limit_mb` ∈ [1, 1024]、title ≤ 200、`tags` 每个 1..50 字符且为字符串、
   `test_cases[].is_sample` 不允许为 `true`。

2. **Solution 校验**：用 Python 跑每个问题的简洁实现，**逐条比对样例输出与判题用例
   `expected_output`**——25/25 样例 + 65/65 判题用例全数对齐。

---

## 写在最后

这 10 道题是 **种子数据**，目的是让 Phase 4（判题）+ Phase 5（前端刷题页）可以立刻
联调。后续可以通过管理后台的 `/admin/problems` 页继续增删改，无需直接编辑本目录。

如果想换题目来源（如 fork LeetCode 题目集），只需按上面的字段约定构造 JSON 即可；
不再需要的题目用管理后台 DELETE 软删（`is_deleted=TRUE`），之后可以从 public 列表
自动下架（[v1.2.6](#) 的 `problem_routes.h::list_problems_handler` 已强制 `include_deleted=false`）。
