// SPDX-License-Identifier: MIT
//
// LiteCode-CPP — 一次性密码哈希生成器 (v1.3.3.7)
//
// 用途:为 `scripts/admin_tool.py` 或一次性 SQL 修复提供一个
//   与生产代码同源的 bcrypt 哈希。
//
// 编译:`lit_gen_password_hash` 已经写到 CMakeLists.txt,和
//   `lit_smoke_check` 一样仅 dev build 才构建,Dockerfile
//   不会包含这个 target。
//
// 用法:
//   lit_gen_password_hash <password>
//   lit_gen_password_hash --verify <password> <existing-hash>
//
// 退出码:
//   0  成功(hash 打印到 stdout,verify 结果打印 OK / FAIL)
//   1  命令行参数错误
//   2  密码不满足强度策略(密码太短 / 缺字母 / 缺数字)
//   3  bcrypt 内部错误
//
// 设计要点:
//   - 只依赖 `auth/password_hash.h`(header-only)+ `litecode_bcrypt`
//     这一个静态库,不拉 HTTP / DB / OpenSSL,链接 < 1 MB。
//   - 不连 MySQL、不写日志、不读 .env → 可以离线运行,适合
//     在任何能编 litecode_server 的机器上重生 hash。
//   - 输出唯一一行,末尾不带换行之外的任何字符,便于
//     `lit_gen_password_hash admin123 | xargs -I{} ...`
//     这种 shell 组合直接吃进 SQL。

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "auth/password_hash.h"

namespace {

int print_usage() {
    std::cerr <<
        "usage:\n"
        "  lit_gen_password_hash <password>                   # print $2b$12$... hash\n"
        "  lit_gen_password_hash --verify <pw> <hash>         # check password matches hash\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            // 生成模式
            const std::string pw(argv[1]);
            const std::string hash = litecode::hash_password(pw);
            std::cout << hash << '\n';
            return 0;
        }
        if (argc == 4 && std::string_view(argv[1]) == "--verify") {
            const std::string pw(argv[2]);
            const std::string hash(argv[3]);
            const bool ok = litecode::verify_password(pw, hash);
            std::cout << (ok ? "OK" : "FAIL") << '\n';
            return ok ? 0 : 2;
        }
        return print_usage();
    } catch (const litecode::PasswordPolicyError& e) {
        std::cerr << "password policy rejected: " << e.what() << '\n';
        return 2;
    } catch (const litecode::PasswordError& e) {
        std::cerr << "bcrypt error: " << e.what() << '\n';
        return 3;
    } catch (const std::exception& e) {
        std::cerr << "unexpected error: " << e.what() << '\n';
        return 3;
    }
}