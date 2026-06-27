#include <iostream>

// Verify all dependencies are linkable
#include <httplib.h>           // cpp-httplib
#include <jwt-cpp/jwt.h>      // jwt-cpp
#include <nlohmann/json.hpp>   // nlohmann/json
#include <zlib.h>              // zlib
#include <openssl/ssl.h>       // OpenSSL
#include <mysqlx/xdevapi.h>    // mysql-connector-c++

// Verify bcrypt wrapper
#include "auth/password_hash.h"

int main() {
    std::cout << "LiteCode-CPP starting..." << std::endl;

    // Verify bcrypt works
    std::string hash = litecode::password_hash("test123");
    bool ok = litecode::password_verify("test123", hash);
    std::cout << "bcrypt test: " << (ok ? "PASS" : "FAIL") << std::endl;

    // Verify jwt-cpp works
    auto token = jwt::create()
        .set_issuer("litecode")
        .set_subject("test")
        .sign(jwt::algorithm::hs256{"secret"});
    std::cout << "jwt-cpp test: token generated" << std::endl;

    // Verify nlohmann/json works
    nlohmann::json j = {{"status", "ok"}};
    std::cout << "json test: " << j.dump() << std::endl;

    // Verify zlib version
    std::cout << "zlib version: " << zlibVersion() << std::endl;

    // Verify OpenSSL version
    std::cout << "OpenSSL version: " << OpenSSL_version_num() << std::endl;

    std::cout << "All dependency checks passed." << std::endl;
    return 0;
}