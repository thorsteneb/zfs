/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2020 by Delphix. All rights reserved.
 */

#include <assert.h>
#include <cityhash.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libzfs_impl.h>
#include <libzfs.h>
#include <libzutil.h>
#include <stddef.h>
#include <stddef.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <umem.h>
#include <unistd.h>
#include <sys/debug.h>
#include <sys/stat.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio_checksum.h>
#include "zfs_fletcher.h"
#include "zstream.h"


#define	MAX_RDT_PHYSMEM_PERCENT		20
#define	SMALLEST_POSSIBLE_MAX_RDT_MB		128

typedef struct redup_entry {
	struct redup_entry	*rde_next;
	uint64_t rde_guid;
	uint64_t rde_object;
	uint64_t rde_offset;
	uint64_t rde_stream_offset;
} redup_entry_t;

typedef struct redup_table {
	redup_entry_t	**redup_hash_array;
	umem_cache_t	*ddecache;
	uint64_t	ddt_count;
	int		numhashbits;
} redup_table_t;

static int
high_order_bit(uint64_t n)
{
	int count;

	for (count = 0; n != 0; count++)
		n >>= 1;
	return (count);
}

static void *
safe_calloc(size_t n)
{
	void *rv = calloc(1, n);
	if (rv == NULL) {
		fprintf(stderr,
		    "Error: could not allocate %u bytes of memory\n",
		    (int)n);
		exit(1);
	}
	return (rv);
}

/*
 * Safe version of fread(), exits on error.
 */
static int
sfread(void *buf, size_t size, FILE *fp)
{
	int rv = fread(buf, size, 1, fp);
	if (rv == 0 && ferror(fp)) {
		(void) fprintf(stderr, "Error while reading file: %s\n",
		    strerror(errno));
		exit(1);
	}
	return (rv);
}

/*
 * Safe version of pread(), exits on error.
 */
static void
spread(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t err = pread(fd, buf, count, offset);
	if (err == -1) {
		(void) fprintf(stderr,
		    "Error while reading file: %s\n",
		    strerror(errno));
		exit(1);
	} else if (err != count) {
		(void) fprintf(stderr,
		    "Error while reading file: short read\n");
		exit(1);
	}
}

static int
dump_record(dmu_replay_record_t *drr, void *payload, int payload_len,
    zio_cksum_t *zc, int outfd)
{
	assert(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum)
	    == sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	fletcher_4_incremental_native(drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum), zc);
	if (drr->drr_type != DRR_BEGIN) {
		assert(ZIO_CHECKSUM_IS_ZERO(&drr->drr_u.
		    drr_checksum.drr_checksum));
		drr->drr_u.drr_checksum.drr_checksum = *zc;
	}
	fletcher_4_incremental_native(&drr->drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), zc);
	if (write(outfd, drr, sizeof (*drr)) == -1)
		return (errno);
	if (payload_len != 0) {
		fletcher_4_incremental_native(payload, payload_len, zc);
		if (write(outfd, payload, payload_len) == -1)
			return (errno);
	}
	return (0);
}

static void
rdt_insert(redup_table_t *rdt,
    uint64_t guid, uint64_t object, uint64_t offset, uint64_t stream_offset)
{
	uint64_t ch = cityhash4(guid, object, offset, 0);
	uint64_t hashcode = BF64_GET(ch, 0, rdt->numhashbits);
	redup_entry_t **rdepp;

	rdepp = &(rdt->redup_hash_array[hashcode]);
	redup_entry_t *rde = umem_cache_alloc(rdt->ddecache, UMEM_NOFAIL);
	rde->rde_next = *rdepp;
	rde->rde_guid = guid;
	rde->rde_object = object;
	rde->rde_offset = offset;
	rde->rde_stream_offset = stream_offset;
	*rdepp = rde;
	rdt->ddt_count++;
}

static void
rdt_lookup(redup_table_t *rdt,
    uint64_t guid, uint64_t object, uint64_t offset,
    uint64_t *stream_offsetp)
{
	uint64_t ch = cityhash4(guid, object, offset, 0);
	uint64_t hashcode = BF64_GET(ch, 0, rdt->numhashbits);

	for (redup_entry_t *rde = rdt->redup_hash_array[hashcode];
	    rde != NULL; rde = rde->rde_next) {
		if (rde->rde_guid == guid &&
		    rde->rde_object == object &&
		    rde->rde_offset == offset) {
			*stream_offsetp = rde->rde_stream_offset;
			return;
		}
	}
	assert(!"could not find expected redup table entry");
}

/*
 * Convert a dedup stream (generated by "zfs send -D") to a
 * non-deduplicated stream.  The entire infd will be converted, including
 * any substreams in a stream package (generated by "zfs send -RD"). The
 * infd must be seekable.
 */
static int
zfs_redup_stream(int infd, int outfd, boolean_t verbose)
{
	int bufsz = SPA_MAXBLOCKSIZE;
	dmu_replay_record_t thedrr = { 0 };
	dmu_replay_record_t *drr = &thedrr;
	redup_table_t rdt;
	zio_cksum_t stream_cksum;
	uint64_t numbuckets;
	uint64_t num_records = 0;
	uint64_t num_write_byref_records = 0;

	if (lseek(infd, 0, SEEK_CUR) == -1) {
		/* Input fd is not seekable. */
		return (ESPIPE);
	}

#ifdef _ILP32
	uint64_t max_rde_size = SMALLEST_POSSIBLE_MAX_RDT_MB << 20;
#else
	uint64_t physmem = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
	uint64_t max_rde_size =
	    MAX((physmem * MAX_RDT_PHYSMEM_PERCENT) / 100,
	    SMALLEST_POSSIBLE_MAX_RDT_MB << 20);
#endif

	numbuckets = max_rde_size / (sizeof (redup_entry_t));

	/*
	 * numbuckets must be a power of 2.  Increase number to
	 * a power of 2 if necessary.
	 */
	if (!ISP2(numbuckets))
		numbuckets = 1ULL << high_order_bit(numbuckets);

	rdt.redup_hash_array = calloc(numbuckets, sizeof (redup_entry_t *));
	rdt.ddecache = umem_cache_create("rde", sizeof (redup_entry_t), 0,
	    NULL, NULL, NULL, NULL, NULL, 0);
	rdt.numhashbits = high_order_bit(numbuckets) - 1;

	char *buf = safe_calloc(bufsz);
	FILE *ofp = fdopen(infd, "r");
	long offset = ftell(ofp);
	while (sfread(drr, sizeof (*drr), ofp) != 0) {
		num_records++;

		/*
		 * We need to regenerate the checksum.
		 */
		if (drr->drr_type != DRR_BEGIN) {
			bzero(&drr->drr_u.drr_checksum.drr_checksum,
			    sizeof (drr->drr_u.drr_checksum.drr_checksum));
		}

		uint64_t payload_size = 0;
		switch (drr->drr_type) {
		case DRR_BEGIN:
		{
			struct drr_begin *drrb = &drr->drr_u.drr_begin;
			int fflags;
			ZIO_SET_CHECKSUM(&stream_cksum, 0, 0, 0, 0);

			assert(drrb->drr_magic == DMU_BACKUP_MAGIC);

			/* clear the DEDUP feature flag for this stream */
			fflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
			fflags &= ~(DMU_BACKUP_FEATURE_DEDUP |
			    DMU_BACKUP_FEATURE_DEDUPPROPS);
			DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, fflags);

			int sz = drr->drr_payloadlen;
			if (sz != 0) {
				if (sz > bufsz) {
					free(buf);
					buf = safe_calloc(sz);
					bufsz = sz;
				}
				(void) sfread(buf, sz, ofp);
			}
			payload_size = sz;
			break;
		}

		case DRR_END:
		{
			struct drr_end *drre = &drr->drr_u.drr_end;
			/*
			 * Use the recalculated checksum, unless this is
			 * the END record of a stream package, which has
			 * no checksum.
			 */
			if (!ZIO_CHECKSUM_IS_ZERO(&drre->drr_checksum))
				drre->drr_checksum = stream_cksum;
			break;
		}

		case DRR_OBJECT:
		{
			struct drr_object *drro = &drr->drr_u.drr_object;

			if (drro->drr_bonuslen > 0) {
				payload_size = DRR_OBJECT_PAYLOAD_SIZE(drro);
				(void) sfread(buf, payload_size, ofp);
			}
			break;
		}

		case DRR_SPILL:
		{
			struct drr_spill *drrs = &drr->drr_u.drr_spill;
			payload_size = DRR_SPILL_PAYLOAD_SIZE(drrs);
			(void) sfread(buf, payload_size, ofp);
			break;
		}

		case DRR_WRITE_BYREF:
		{
			struct drr_write_byref drrwb =
			    drr->drr_u.drr_write_byref;

			num_write_byref_records++;

			/*
			 * Look up in hash table by drrwb->drr_refguid,
			 * drr_refobject, drr_refoffset.  Replace this
			 * record with the found WRITE record, but with
			 * drr_object,drr_offset,drr_toguid replaced with ours.
			 */
			uint64_t stream_offset;
			rdt_lookup(&rdt, drrwb.drr_refguid,
			    drrwb.drr_refobject, drrwb.drr_refoffset,
			    &stream_offset);

			spread(infd, drr, sizeof (*drr), stream_offset);

			assert(drr->drr_type == DRR_WRITE);
			struct drr_write *drrw = &drr->drr_u.drr_write;
			assert(drrw->drr_toguid == drrwb.drr_refguid);
			assert(drrw->drr_object == drrwb.drr_refobject);
			assert(drrw->drr_offset == drrwb.drr_refoffset);

			payload_size = DRR_WRITE_PAYLOAD_SIZE(drrw);
			spread(infd, buf, payload_size,
			    stream_offset + sizeof (*drr));

			drrw->drr_toguid = drrwb.drr_toguid;
			drrw->drr_object = drrwb.drr_object;
			drrw->drr_offset = drrwb.drr_offset;
			break;
		}

		case DRR_WRITE:
		{
			struct drr_write *drrw = &drr->drr_u.drr_write;
			payload_size = DRR_WRITE_PAYLOAD_SIZE(drrw);
			(void) sfread(buf, payload_size, ofp);

			rdt_insert(&rdt, drrw->drr_toguid,
			    drrw->drr_object, drrw->drr_offset, offset);
			break;
		}

		case DRR_WRITE_EMBEDDED:
		{
			struct drr_write_embedded *drrwe =
			    &drr->drr_u.drr_write_embedded;
			payload_size =
			    P2ROUNDUP((uint64_t)drrwe->drr_psize, 8);
			(void) sfread(buf, payload_size, ofp);
			break;
		}

		case DRR_FREEOBJECTS:
		case DRR_FREE:
		case DRR_OBJECT_RANGE:
			break;

		default:
			(void) fprintf(stderr, "INVALID record type 0x%x\n",
			    drr->drr_type);
			/* should never happen, so assert */
			assert(B_FALSE);
		}

		if (feof(ofp)) {
			fprintf(stderr, "Error: unexpected end-of-file\n");
			exit(1);
		}
		if (ferror(ofp)) {
			fprintf(stderr, "Error while reading file: %s\n",
			    strerror(errno));
			exit(1);
		}

		/*
		 * We need to recalculate the checksum, and it needs to be
		 * initially zero to do that.  BEGIN records don't have
		 * a checksum.
		 */
		if (drr->drr_type != DRR_BEGIN) {
			bzero(&drr->drr_u.drr_checksum.drr_checksum,
			    sizeof (drr->drr_u.drr_checksum.drr_checksum));
		}
		if (dump_record(drr, buf, payload_size,
		    &stream_cksum, outfd) != 0)
			break;
		if (drr->drr_type == DRR_END) {
			/*
			 * Typically the END record is either the last
			 * thing in the stream, or it is followed
			 * by a BEGIN record (which also zero's the cheksum).
			 * However, a stream package ends with two END
			 * records.  The last END record's checksum starts
			 * from zero.
			 */
			ZIO_SET_CHECKSUM(&stream_cksum, 0, 0, 0, 0);
		}
		offset = ftell(ofp);
	}

	if (verbose) {
		char mem_str[16];
		zfs_nicenum(rdt.ddt_count * sizeof (redup_entry_t),
		    mem_str, sizeof (mem_str));
		fprintf(stderr, "converted stream with %llu total records, "
		    "including %llu dedup records, using %sB memory.\n",
		    (long long)num_records,
		    (long long)num_write_byref_records,
		    mem_str);
	}

	umem_cache_destroy(rdt.ddecache);
	free(rdt.redup_hash_array);
	free(buf);
	(void) fclose(ofp);

	return (0);
}

int
zstream_do_redup(int argc, char *argv[])
{
	boolean_t verbose = B_FALSE;
	char c;

	while ((c = getopt(argc, argv, "v")) != -1) {
		switch (c) {
		case 'v':
			verbose = B_TRUE;
			break;
		case '?':
			(void) fprintf(stderr, "invalid option '%c'\n",
			    optopt);
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	const char *filename = argv[0];

	if (isatty(STDOUT_FILENO)) {
		(void) fprintf(stderr,
		    "Error: Stream can not be written to a terminal.\n"
		    "You must redirect standard output.\n");
		return (1);
	}

	int fd = open(filename, O_RDONLY);
	if (fd == -1) {
		(void) fprintf(stderr,
		    "Error while opening file '%s': %s\n",
		    filename, strerror(errno));
		exit(1);
	}

	fletcher_4_init();
	int err = zfs_redup_stream(fd, STDOUT_FILENO, verbose);
	fletcher_4_fini();

	close(fd);

	return (err != 0);
}
