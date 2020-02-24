/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2013 Saso Kiselkov.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2016 by Delphix. All rights reserved.
 */
#include <sys/zfs_context.h>
#include <sys/zio.h>
#include <sys/blake3.h>
#include <sys/zfs_context.h>	/* For CTASSERT() */
#include <sys/abd.h>

#pragma GCC diagnostic ignored "-Wframe-larger-than="

static int
blake3_incremental(void *buf, size_t size, void *arg)
{
	blake3_hasher *ctx = arg;
	blake3_hasher_update(ctx, buf, size);
	return (0);
}

/*
 * Native zio_checksum interface for the BLAKE3 hash function.
 */
/*ARGSUSED*/
void
abd_checksum_blake3_native(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	blake3_hasher	ctx;

	blake3_hasher_init(&ctx);
	(void) abd_iterate_func(abd, 0, size, blake3_incremental, &ctx);
	blake3_hasher_finalize(&ctx, (uint8_t *)zcp->zc_word,
	    sizeof (zcp->zc_word));
}

/*
 * Byteswapped zio_checksum interface for the BLAKE3 hash function.
 */
void
abd_checksum_blake3_byteswap(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	zio_cksum_t	tmp;

	abd_checksum_blake3_native(abd, size, ctx_template, &tmp);
	zcp->zc_word[0] = BSWAP_64(zcp->zc_word[0]);
	zcp->zc_word[1] = BSWAP_64(zcp->zc_word[1]);
	zcp->zc_word[2] = BSWAP_64(zcp->zc_word[2]);
	zcp->zc_word[3] = BSWAP_64(zcp->zc_word[3]);
}
