/*
 * bcrypt - password hashing library
 *
 * Derived from OpenBSD bcrypt implementation.
 * Adapted for cross-platform use with stdint.h types.
 *
 * Copyright (c) 1997 Niels Provos and others
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in source code must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * NOTE: This file is named litecode_bcrypt.h (not bcrypt.h) to avoid
 * a name collision with Windows SDK's <bcrypt.h> which is included
 * indirectly via OpenSSL headers.
 */

#ifndef LITECODE_BCRYPT_H_
#define LITECODE_BCRYPT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BCRYPT_HASHSIZE		64
#define BCRYPT_GENSALT_OUTPUT_SIZE	32

/* Generate a salt string for bcrypt hashing */
int bcrypt_gensalt(int workfactor, char output[BCRYPT_GENSALT_OUTPUT_SIZE]);

/* Hash a password with the given salt */
int bcrypt_hashpass(const char *key, const char *salt,
    char output[BCRYPT_HASHSIZE]);

/* Verify a password against a stored hash; returns 0 on match */
int bcrypt_checkpass(const char *key, const char *hash);

#ifdef __cplusplus
}
#endif

#endif /* LITECODE_BCRYPT_H_ */