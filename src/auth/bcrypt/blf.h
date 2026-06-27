/*	$OpenBSD: blf.h,v 1.8 2021/11/29 19:37:27 tb Exp $	*/

/*
 * Blowfish - a fast block cipher
 *
 * Derived from OpenBSD source by Niels Provos.
 * Adapted for cross-platform use with stdint.h types.
 *
 * Copyright (c) 1997, 2012 Niels Provos and others
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
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
 */

#ifndef _BLF_H_
#define _BLF_H_

#include <stdint.h>

/* Number of rounds */
#define BLF_N	16

/* Blowfish context */
typedef struct blf_ctx {
	uint32_t S[4][256];	/* S-boxes */
	uint32_t P[18];		/* Subkeys */
} blf_ctx;

/* Blowfish key schedule */
void Blowfish_initstate(blf_ctx *c);
void Blowfish_expandstate(blf_ctx *c, const uint8_t *data, uint16_t databytes,
    const uint8_t *key, uint16_t keybytes);
void Blowfish_expand0state(blf_ctx *c, const uint8_t *key, uint16_t keybytes);
uint32_t Blowfish_stream2word(const uint8_t *data, uint16_t databytes,
    uint16_t *current);
void Blowfish_enc(blf_ctx *c, uint32_t *data);
void Blowfish_dec(blf_ctx *c, uint32_t *data);
void blf_enc(blf_ctx *c, uint32_t *data, uint16_t blocks);
void blf_dec(blf_ctx *c, uint32_t *data, uint16_t blocks);

#endif /* _BLF_H_ */