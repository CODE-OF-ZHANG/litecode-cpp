/*	$OpenBSD: bcrypt.c,v 1.58 2020/07/06 13:33:05 pirofti Exp $	*/

/*
 * Copyright (c) 2014 Ted Unangst <tedu@openbsd.org>
 * Copyright (c) 1997 Niels Provos <provos@umich.edu>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA, OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This password hashing algorithm was designed by David Mazieres
 * <dm@lcs.mit.edu> and works as follows:
 *
 * 1. state := InitState ()
 * 2. state := ExpandKey (state, salt, password)
 * 3. REPEAT rounds:
 *      state := ExpandKey (state, 0, password)
 *      state := ExpandKey (state, 0, salt)
 * 4. ctext := "OrpheanBeholderScryDoubt"
 * 5. REPEAT 64:
 *      ctext := Encrypt_ECB (state, ctext);
 * 6. RETURN Concatenate (salt, ctext);
 *
 * Adapted for cross-platform use:
 * - Replaced arc4random_buf with portable secure_random()
 * - Replaced explicit_bzero with portable version
 * - Replaced timingsafe_bcmp with portable constant-time comparison
 * - Use bcrypt_hashpass naming (not bcrypt_hashpw)
 * - Use stdint.h types (uint8_t, uint32_t)
 * - Use "blf.h" and "bcrypt.h" with local quotes
 */

#ifdef _WIN32
/* Windows: use CryptoAPI for secure random number generation.
 * We declare the needed types and functions directly to avoid including
 * <wincrypt.h>, which transitively includes the Windows <bcrypt.h>
 * (CryptoNG). That header name conflicts with our local bcrypt.h since
 * the -I compiler flag causes our header to shadow the system one. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef ULONG_PTR HCRYPTPROV;
#define PROV_RSA_FULL          1
#define CRYPT_VERIFYCONTEXT    0xF0000000U
#define CRYPT_SILENT           0x00000040U

BOOL WINAPI CryptAcquireContextW(HCRYPTPROV *phProv, LPCWSTR szContainer,
    LPCWSTR szProvider, DWORD dwProvType, DWORD dwFlags);
BOOL WINAPI CryptGenRandom(HCRYPTPROV hProv, DWORD dwLen, BYTE *pbBuffer);
BOOL WINAPI CryptReleaseContext(HCRYPTPROV hProv, DWORD dwFlags);
#ifdef UNICODE
#define CryptAcquireContext CryptAcquireContextW
#else
#define CryptAcquireContext CryptAcquireContextA
BOOL WINAPI CryptAcquireContextA(HCRYPTPROV *phProv, LPCSTR szContainer,
    LPCSTR szProvider, DWORD dwProvType, DWORD dwFlags);
#endif
#endif

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "blf.h"
#include "litecode_bcrypt.h"

/* This implementation is adaptable to current computing power.
 * You can have up to 2^31 rounds which should be enough for some
 * time to come.
 */

#define BCRYPT_VERSION	'2'
#define BCRYPT_MAXSALT	16	/* Precomputation is just so nice */
#define BCRYPT_WORDS	6	/* Ciphertext words */
#define BCRYPT_MINLOGROUNDS 4	/* we have log2(rounds) in salt */

#define	BCRYPT_SALTSPACE	(7 + (BCRYPT_MAXSALT * 4 + 2) / 3 + 1)
#define	BCRYPT_HASHSPACE	61

/* Base64 encoding/decoding tables */

static const uint8_t Base64Code[] =
"./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static const uint8_t index_64[128] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 0, 1, 54, 55,
	56, 57, 58, 59, 60, 61, 62, 63, 255, 255,
	255, 255, 255, 255, 255, 2, 3, 4, 5, 6,
	7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
	255, 255, 255, 255, 255, 255, 28, 29, 30,
	31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
	51, 52, 53, 255, 255, 255, 255, 255
};

#define CHAR64(c)  ((c) > 127 ? 255 : index_64[(c)])

/*
 * Portable secure random number generation.
 * On Windows: uses CryptGenRandom (CryptoAPI).
 * On Linux/macOS: reads from /dev/urandom.
 * Returns 0 on success, -1 on failure.
 */
static int
secure_random(uint8_t *buf, size_t buflen)
{
	if (buf == NULL || buflen == 0)
		return -1;

#ifdef _WIN32
	HCRYPTPROV hProv;
	if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL,
	    CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
		return -1;
	if (!CryptGenRandom(hProv, (DWORD)buflen, buf)) {
		CryptReleaseContext(hProv, 0);
		return -1;
	}
	CryptReleaseContext(hProv, 0);
	return 0;
#else
	int fd;
	ssize_t nread;
	size_t total = 0;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;

	while (total < buflen) {
		nread = read(fd, buf + total, buflen - total);
		if (nread < 0) {
			close(fd);
			return -1;
		}
		if (nread == 0) {
			/* Should not happen for /dev/urandom */
			close(fd);
			return -1;
		}
		total += (size_t)nread;
	}

	close(fd);
	return 0;
#endif
}

/*
 * Portable explicit_bzero: securely zero memory, preventing the compiler
 * from optimizing away the clear operation.
 */
static void
explicit_bzero_portable(void *s, size_t len)
{
	volatile uint8_t *p = (volatile uint8_t *)s;
	while (len--)
		*p++ = 0;
}

/*
 * Portable constant-time memory comparison.
 * Returns 0 if equal, non-zero if different.
 */
static int
timingsafe_bcmp_portable(const void *b1, const void *b2, size_t n)
{
	const uint8_t *p1 = (const uint8_t *)b1;
	const uint8_t *p2 = (const uint8_t *)b2;
	uint8_t ret = 0;

	for (size_t i = 0; i < n; i++)
		ret |= p1[i] ^ p2[i];

	return ret;
}

/*
 * Turn len bytes of data into base64 encoded data.
 * This works without = padding.
 */
static int
encode_base64(char *b64buffer, const uint8_t *data, size_t len)
{
	char *bp = b64buffer;
	const uint8_t *p = data;
	uint8_t c1, c2;

	while (p < data + len) {
		c1 = *p++;
		*bp++ = Base64Code[(c1 >> 2)];
		c1 = (c1 & 0x03) << 4;
		if (p >= data + len) {
			*bp++ = Base64Code[c1];
			break;
		}
		c2 = *p++;
		c1 |= (c2 >> 4) & 0x0f;
		*bp++ = Base64Code[c1];
		c1 = (c2 & 0x0f) << 2;
		if (p >= data + len) {
			*bp++ = Base64Code[c1];
			break;
		}
		c2 = *p++;
		c1 |= (c2 >> 6) & 0x03;
		*bp++ = Base64Code[c1];
		*bp++ = Base64Code[c2 & 0x3f];
	}
	*bp = '\0';
	return 0;
}

/*
 * read buflen (after decoding) bytes of data from b64data
 */
static int
decode_base64(uint8_t *buffer, size_t len, const char *b64data)
{
	uint8_t *bp = buffer;
	const uint8_t *p = (const uint8_t *)b64data;
	uint8_t c1, c2, c3, c4;

	while (bp < buffer + len) {
		c1 = CHAR64(*p);
		/* Invalid data */
		if (c1 == 255)
			return -1;

		c2 = CHAR64(*(p + 1));
		if (c2 == 255)
			return -1;

		*bp++ = (c1 << 2) | ((c2 & 0x30) >> 4);
		if (bp >= buffer + len)
			break;

		c3 = CHAR64(*(p + 2));
		if (c3 == 255)
			return -1;

		*bp++ = ((c2 & 0x0f) << 4) | ((c3 & 0x3c) >> 2);
		if (bp >= buffer + len)
			break;

		c4 = CHAR64(*(p + 3));
		if (c4 == 255)
			return -1;
		*bp++ = ((c3 & 0x03) << 6) | c4;

		p += 4;
	}
	return 0;
}

/*
 * Generates a salt for this version of crypt.
 */
int
bcrypt_gensalt(int workfactor, char output[BCRYPT_GENSALT_OUTPUT_SIZE])
{
	uint8_t csalt[BCRYPT_MAXSALT];
	int log_rounds = workfactor;

	if (log_rounds < 4)
		log_rounds = 4;
	else if (log_rounds > 31)
		log_rounds = 31;

	if (secure_random(csalt, sizeof(csalt)) != 0)
		return -1;

	snprintf(output, BCRYPT_GENSALT_OUTPUT_SIZE, "$2b$%2.2u$", log_rounds);
	encode_base64(output + 7, csalt, sizeof(csalt));

	return 0;
}

/*
 * The core bcrypt function
 */
static int
bcrypt_hashpass_internal(const char *key, const char *salt, char *encrypted,
    size_t encryptedlen)
{
	blf_ctx state;
	uint32_t rounds, i, k;
	uint16_t j;
	size_t key_len;
	uint8_t salt_len, logr, minor;
	uint8_t ciphertext[4 * BCRYPT_WORDS] = "OrpheanBeholderScryDoubt";
	uint8_t csalt[BCRYPT_MAXSALT];
	uint32_t cdata[BCRYPT_WORDS];

	if (encryptedlen < BCRYPT_HASHSPACE)
		goto inval;

	/* Check and discard "$" identifier */
	if (salt[0] != '$')
		goto inval;
	salt += 1;

	if (salt[0] != BCRYPT_VERSION)
		goto inval;

	/* Check for minor versions */
	switch ((minor = salt[1])) {
	case 'a':
		key_len = (uint8_t)(strlen(key) + 1);
		break;
	case 'b':
		/* strlen() returns a size_t, but the function calls
		 * below result in implicit casts to a narrower integer
		 * type, so cap key_len at the actual maximum supported
		 * length here to avoid integer wraparound */
		key_len = strlen(key);
		if (key_len > 72)
			key_len = 72;
		key_len++; /* include the NUL */
		break;
	default:
		goto inval;
	}
	if (salt[2] != '$')
		goto inval;
	/* Discard version + "$" identifier */
	salt += 3;

	/* Check and parse num rounds */
	if (!isdigit((unsigned char)salt[0]) ||
	    !isdigit((unsigned char)salt[1]) || salt[2] != '$')
		goto inval;
	logr = (salt[1] - '0') + ((salt[0] - '0') * 10);
	if (logr < BCRYPT_MINLOGROUNDS || logr > 31)
		goto inval;
	/* Computer power doesn't increase linearly, 2^x should be fine */
	rounds = 1U << logr;

	/* Discard num rounds + "$" identifier */
	salt += 3;

	if (strlen(salt) * 3 / 4 < BCRYPT_MAXSALT)
		goto inval;

	/* We don't want the base64 salt but the raw data */
	if (decode_base64(csalt, BCRYPT_MAXSALT, salt))
		goto inval;
	salt_len = BCRYPT_MAXSALT;

	/* Setting up S-Boxes and Subkeys */
	Blowfish_initstate(&state);
	Blowfish_expandstate(&state, csalt, salt_len,
	    (const uint8_t *)key, (uint16_t)key_len);
	for (k = 0; k < rounds; k++) {
		Blowfish_expand0state(&state, (const uint8_t *)key, (uint16_t)key_len);
		Blowfish_expand0state(&state, csalt, salt_len);
	}

	/* This can be precomputed later */
	j = 0;
	for (i = 0; i < BCRYPT_WORDS; i++)
		cdata[i] = Blowfish_stream2word(ciphertext, 4 * BCRYPT_WORDS, &j);

	/* Now do the encryption */
	for (k = 0; k < 64; k++)
		blf_enc(&state, cdata, BCRYPT_WORDS / 2);

	for (i = 0; i < BCRYPT_WORDS; i++) {
		ciphertext[4 * i + 3] = cdata[i] & 0xff;
		cdata[i] = cdata[i] >> 8;
		ciphertext[4 * i + 2] = cdata[i] & 0xff;
		cdata[i] = cdata[i] >> 8;
		ciphertext[4 * i + 1] = cdata[i] & 0xff;
		cdata[i] = cdata[i] >> 8;
		ciphertext[4 * i + 0] = cdata[i] & 0xff;
	}

	snprintf(encrypted, 8, "$2%c$%2.2u$", minor, logr);
	encode_base64(encrypted + 7, csalt, BCRYPT_MAXSALT);
	encode_base64(encrypted + 7 + 22, ciphertext, 4 * BCRYPT_WORDS - 1);
	explicit_bzero_portable(&state, sizeof(state));
	explicit_bzero_portable(ciphertext, sizeof(ciphertext));
	explicit_bzero_portable(csalt, sizeof(csalt));
	explicit_bzero_portable(cdata, sizeof(cdata));
	return 0;

inval:
	errno = EINVAL;
	return -1;
}

/*
 * Hash a password using the given salt.
 * Public API matching bcrypt.h declaration.
 */
int
bcrypt_hashpass(const char *key, const char *salt, char output[BCRYPT_HASHSIZE])
{
	return bcrypt_hashpass_internal(key, salt, output, BCRYPT_HASHSIZE);
}

/*
 * Verify a password against a stored hash.
 * Returns 0 on match, non-zero on failure.
 */
int
bcrypt_checkpass(const char *key, const char *hash)
{
	char computed[BCRYPT_HASHSIZE];

	if (bcrypt_hashpass_internal(key, hash, computed, sizeof(computed)) != 0)
		return -1;

	if (strlen(computed) != strlen(hash) ||
	    timingsafe_bcmp_portable(computed, hash, strlen(hash)) != 0) {
		explicit_bzero_portable(computed, sizeof(computed));
		errno = EACCES;
		return -1;
	}

	explicit_bzero_portable(computed, sizeof(computed));
	return 0;
}