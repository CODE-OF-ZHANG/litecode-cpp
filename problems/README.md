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
| `special` | Special Judge（v1.3.1 闭环：admin 上传 C++ SPJ → judge.sh 编译 → 按 rc 判 AC/WA/SE；无 SPJ 兜底 WA） |
| `ignore_case`（v1.3.1+） | ASCII 大小写不敏感（`a-z ⇄ A-Z` 翻转后 cmp；UTF-8 多字节字符按原样 cmp；与 Codeforces / AtCoder 的 case-insensitive 输出一致） |
| `ignore_all_whitespace`（v1.3.1+） | 行内多空格折叠（`[ \t]+ → 单空格`）+ 空行忽略；比 `ignore_trailing` 严格（行内也折叠，与 ignore_trailing 在末尾空白上重叠） |

> 注：v1.2.10 实现的 `parse_test_cases_array` 在 wire 协议上接受 `float_epsilon`
> 字段，但实现层固定绑 `1e-8` 默认值（详见 v1.2.10 changelog + header 注释）。
> 本仓库内 `sqrt-x.json` 在 `float_eps` 用例中给出 `1e-4` 容差——其作用是说明题面期望，
> 实际入库值仍为 DB 默认值，未来 §8.1 schema 升级后再做 binding 透传。

### Special Judge（v1.3.1 闭环）

`judge_type=special` 的题目需要管理员上传一道 C++ **Special Judge 源程序**，
由 `judge.sh` 在判题时编译并按 SPJ 的退出码判定 AC/WA/SE。

**SPJ 字段在 JSON 中的位置**（与 `slug` / `samples` 同级，写到题目 JSON 即可随批量导入
进入 `problem_special_judges` 表）：

```jsonc
{
  "slug": "floating-point-sum",
  "title": "浮点求和（容差 1e-4）",
  "difficulty": "medium",
  "description": "## 题目描述\n\n求和并按 1e-4 误差比较。",
  "samples": [{ "input": "3\n1.0 2.0 3.0\n", "output": "6.0\n" }],
  "test_cases": [
    {
      "input": "3\n1.0 2.0 3.0\n",
      "expected_output": "6.0\n",
      "judge_type": "special",
      "float_epsilon": 0.0001
    }
  ],
  // ↓ v1.3.1 新增：SPJ 源码（与 slug/samples 同级；非 sample 的字段）
  "special_judge_source": "// SPJ: byte-compare + 1e-4 浮点容差\n"
                          "#include <cstdio>\n"
                          "#include <cstdlib>\n"
                          "#include <cmath>\n"
                          "int main(int argc, char** argv) {\n"
                          "    if (argc != 4) return 2;\n"
                          "    FILE* e = std::fopen(argv[2], \"rb\");\n"
                          "    FILE* a = std::fopen(argv[3], \"rb\");\n"
                          "    if (!e || !a) return 2;\n"
                          "    // ... 浮点解析 + 误差比较 ...\n"
                          "    return 0;  // AC；1 = WA\n"
                          "}\n",
  "special_judge_language": "cpp"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `special_judge_source` | 选填 | C++ SPJ 源程序；空 = 题未挂 SPJ，judge.sh 走 WA 兜底。JSON 导入时入库到 `problem_special_judges.source`（MEDIUMTEXT 16MB） |
| `special_judge_language` | 选填 | 默认 `"cpp"`；当前镜像只支持 C++，其它值 400 INVALID_INPUT |

> **批量导入路径**：`admin_bulk_import_routes.h::detail::process_one_file` 已识别
> `special_judge_source` / `special_judge_language` 并写入 V010 表（见
> `tests/unit/test_admin_special_judge.cpp::UpsertIdempotentSameContent` 验证）。
>
> **单题上传路径**：POST/PUT `/admin/problems/:slug` 不接受 SPJ 字段（保持题目
> 元数据与 SPJ 解耦），SPJ 必须通过 PUT `/admin/problems/:slug/special-judge`
> 单独上传，幂等 upsert（v1.3.1 加的 3 个 admin 端点）。
>
> **admin 端 source 大小 clamp**：256 KB（`kMaxSpjSourceLenAdmin`，比 repo ceiling 16MB
> 紧很多，因为 admin 上传通常很小 + 编译时间可控）。超 256KB → 400 INVALID_INPUT。
> 仓库内 MEDIUMTEXT 仍 16MB，可由运维手工 `INSERT INTO problem_special_judges`
> 应急超过 256KB 的 SPJ（v1.3.2+ 计划抬高 admin cap）。

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
