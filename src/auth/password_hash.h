#pragma once

#include <string>
#include "litecode_bcrypt.h" // C interface from src/auth/bcrypt/litecode_bcrypt.h

namespace litecode {

/// Generate a bcrypt hash for a password.
/// Uses cost factor 12 (4096 iterations) by default.
/// @param password  The plaintext password to hash.
/// @return          The bcrypt hash string (e.g., "$2b$12$...").
inline std::string password_hash(const std::string& password) {
    char salt[BCRYPT_GENSALT_OUTPUT_SIZE];
    if (bcrypt_gensalt(12, salt) != 0) {
        return "";
    }

    char hash[BCRYPT_HASHSIZE];
    if (bcrypt_hashpass(password.c_str(), salt, hash) != 0) {
        return "";
    }

    return std::string(hash);
}

/// Verify a password against a bcrypt hash.
/// @param password  The plaintext password to check.
/// @param hash       The stored bcrypt hash string.
/// @return           true if the password matches, false otherwise.
inline bool password_verify(const std::string& password, const std::string& hash) {
    return bcrypt_checkpass(password.c_str(), hash.c_str()) == 0;
}

} // namespace litecode
