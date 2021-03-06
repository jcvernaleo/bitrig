/*	$OpenBSD: subr_disk.c,v 1.183 2015/05/09 17:11:26 krw Exp $	*/
/*	$NetBSD: subr_disk.c,v 1.17 1996/03/16 23:17:08 christos Exp $	*/

/*
 * Copyright (c) 1995 Jason R. Thorpe.  All rights reserved.
 * Copyright (c) 1982, 1986, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)ufs_disksubr.c	8.5 (Berkeley) 1/21/94
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/fcntl.h>
#include <sys/buf.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <sys/device.h>
#include <sys/time.h>
#include <sys/disklabel.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/disk.h>
#include <sys/reboot.h>
#include <sys/dkio.h>
#include <sys/vnode.h>
#include <sys/task.h>

#include <sys/socket.h>
#include <sys/socketvar.h>

#include <net/if.h>

#include <dev/rndvar.h>
#include <dev/cons.h>

#include <lib/libz/zlib.h>

#include "softraid.h"

#ifdef DEBUG
#define DPRINTF(x...)	printf(x)
#else
#define DPRINTF(x...)
#endif

/*
 * A global list of all disks attached to the system.  May grow or
 * shrink over time.
 */
struct	disklist_head disklist;	/* TAILQ_HEAD */
int	disk_count;		/* number of drives in global disklist */
int	disk_change;		/* set if a disk has been attached/detached
				 * since last we looked at this variable. This
				 * is reset by hw_sysctl()
				 */

u_char	bootduid[8];		/* DUID of boot disk. */
u_char	rootduid[8];		/* DUID of root disk. */

/* softraid callback, do not use! */
void (*softraid_disk_attach)(struct disk *, int);

void sr_map_root(void);

bool tmpfsrd_present(void);

struct disk_attach_task {
	struct task task;
	struct disk *dk;
};

void disk_attach_callback(void *);

struct device *setroot_swapgeneric(struct device *, dev_t *);

/*
 * Compute checksum for disk label.
 */
u_int
dkcksum(struct disklabel *lp)
{
	u_int16_t *start, *end;
	u_int16_t sum = 0;

	start = (u_int16_t *)lp;
	end = (u_int16_t *)&lp->d_partitions[lp->d_npartitions];
	while (start < end)
		sum ^= *start++;
	return (sum);
}

int
initdisklabel(struct disklabel *lp)
{
	int i;

	/* minimal requirements for archetypal disk label */
	if (lp->d_secsize < DEV_BSIZE)
		lp->d_secsize = DEV_BSIZE;
	if (DL_GETDSIZE(lp) == 0)
		DL_SETDSIZE(lp, MAXDISKSIZE);
	if (lp->d_secpercyl == 0)
		return (ERANGE);
	lp->d_npartitions = MAXPARTITIONS;
	for (i = 0; i < RAW_PART; i++) {
		DL_SETPSIZE(&lp->d_partitions[i], 0);
		DL_SETPOFFSET(&lp->d_partitions[i], 0);
	}
	if (DL_GETPSIZE(&lp->d_partitions[RAW_PART]) == 0)
		DL_SETPSIZE(&lp->d_partitions[RAW_PART], DL_GETDSIZE(lp));
	DL_SETPOFFSET(&lp->d_partitions[RAW_PART], 0);
	DL_SETBSTART(lp, 0);
	DL_SETBEND(lp, DL_GETDSIZE(lp));
	lp->d_version = 1;
	lp->d_bbsize = 8192;
	lp->d_sbsize = 64*1024;			/* XXX ? */
	return (0);
}

/*
 * Check an incoming block to make sure it is a disklabel, convert it to
 * a newer version if needed, etc etc.
 */
int
checkdisklabel(void *rlp, struct disklabel *lp, u_int64_t boundstart,
    u_int64_t boundend)
{
	struct disklabel *dlp = rlp;
	struct partition *pp;
	u_int64_t disksize;
	int error = 0;
	int i;

	if (dlp->d_magic != DISKMAGIC || dlp->d_magic2 != DISKMAGIC)
		error = ENOENT;	/* no disk label */
	else if (dlp->d_npartitions > MAXPARTITIONS)
		error = E2BIG;	/* too many partitions */
	else if (dlp->d_secpercyl == 0 || dlp->d_version != 1)
		error = EINVAL;	/* invalid label */
	else if (dlp->d_secsize == 0)
		error = ENOSPC;	/* disk too small */
	else if (dkcksum(dlp) != 0)
		error = EINVAL;	/* incorrect checksum */

	if (error) {
		u_int16_t *start, *end, sum = 0;

		/* If it is byte-swapped, attempt to convert it */
		if (swap32(dlp->d_magic) != DISKMAGIC ||
		    swap32(dlp->d_magic2) != DISKMAGIC ||
		    swap16(dlp->d_npartitions) > MAXPARTITIONS ||
		    swap16(dlp->d_version) != 1)
			return (error);

		/*
		 * Need a byte-swap aware dkcksum variant
		 * inlined, because dkcksum uses a sub-field
		 */
		start = (u_int16_t *)dlp;
		end = (u_int16_t *)&dlp->d_partitions[
		    swap16(dlp->d_npartitions)];
		while (start < end)
			sum ^= *start++;
		if (sum != 0)
			return (error);

		error = 0;

		dlp->d_magic = swap32(dlp->d_magic);
		dlp->d_type = swap16(dlp->d_type);
		dlp->d_subtype = swap16(dlp->d_subtype);

		/* d_typename and d_packname are strings */

		dlp->d_secsize = swap32(dlp->d_secsize);
		dlp->d_nsectors = swap32(dlp->d_nsectors);
		dlp->d_ntracks = swap32(dlp->d_ntracks);
		dlp->d_ncylinders = swap32(dlp->d_ncylinders);
		dlp->d_secpercyl = swap32(dlp->d_secpercyl);
		dlp->d_secperunit = swap32(dlp->d_secperunit);

		/* d_uid is a string */

		dlp->d_acylinders = swap32(dlp->d_acylinders);

		dlp->d_flags = swap32(dlp->d_flags);

		for (i = 0; i < NDDATA; i++)
			dlp->d_drivedata[i] = swap32(dlp->d_drivedata[i]);

		dlp->d_secperunith = swap16(dlp->d_secperunith);
		dlp->d_version = swap16(dlp->d_version);

		for (i = 0; i < NSPARE; i++)
			dlp->d_spare[i] = swap32(dlp->d_spare[i]);

		dlp->d_magic2 = swap32(dlp->d_magic2);

		dlp->d_npartitions = swap16(dlp->d_npartitions);
		dlp->d_bbsize = swap32(dlp->d_bbsize);
		dlp->d_sbsize = swap32(dlp->d_sbsize);

		for (i = 0; i < MAXPARTITIONS; i++) {
			pp = &dlp->d_partitions[i];
			pp->p_size = swap32(pp->p_size);
			pp->p_offset = swap32(pp->p_offset);
			pp->p_offseth = swap16(pp->p_offseth);
			pp->p_sizeh = swap16(pp->p_sizeh);
			pp->p_cpg = swap16(pp->p_cpg);
		}

		dlp->d_checksum = 0;
		dlp->d_checksum = dkcksum(dlp);
	}

	/* XXX should verify lots of other fields and whine a lot */

	/* Initial passed in lp contains the real disk size. */
	disksize = DL_GETDSIZE(lp);

	if (lp != dlp)
		*lp = *dlp;

#ifdef DEBUG
	if (DL_GETDSIZE(lp) != disksize)
		printf("on-disk disklabel has incorrect disksize (%llu)\n",
		    DL_GETDSIZE(lp));
	if (DL_GETPSIZE(&lp->d_partitions[RAW_PART]) != disksize)
		printf("on-disk disklabel RAW_PART has incorrect size (%llu)\n",
		    DL_GETPSIZE(&lp->d_partitions[RAW_PART]));
	if (DL_GETPOFFSET(&lp->d_partitions[RAW_PART]) != 0)
		printf("on-disk disklabel RAW_PART offset != 0 (%llu)\n",
		    DL_GETPOFFSET(&lp->d_partitions[RAW_PART]));
#endif
	DL_SETDSIZE(lp, disksize);
	DL_SETPSIZE(&lp->d_partitions[RAW_PART], disksize);
	DL_SETPOFFSET(&lp->d_partitions[RAW_PART], 0);
	DL_SETBSTART(lp, boundstart);
	DL_SETBEND(lp, boundend < DL_GETDSIZE(lp) ? boundend : DL_GETDSIZE(lp));

	lp->d_checksum = 0;
	lp->d_checksum = dkcksum(lp);
	return (0);
}

/*
 * If dos partition table requested, attempt to load it and
 * find disklabel inside a DOS partition. Return buffer
 * for use in signalling errors if requested.
 *
 * We would like to check if each MBR has a valid BOOT_MAGIC, but
 * we cannot because it doesn't always exist. So.. we assume the
 * MBR is valid.
 */
int
readdoslabel(struct buf *bp, void (*strat)(struct buf *),
    struct disklabel *lp, daddr_t *partoffp, int spoofonly)
{
	u_int64_t dospartoff = 0, dospartend = DL_GETBEND(lp);
	int i, ourpart = -1, wander = 1, n = 0, loop = 0, offset;
	struct dos_partition dp[NDOSPART], *dp2;
	daddr_t part_blkno = DOSBBSECTOR;
	u_int32_t extoff = 0;
	int error;

	if (lp->d_secpercyl == 0)
		return (EINVAL);	/* invalid label */
	if (lp->d_secsize == 0)
		return (ENOSPC);	/* disk too small */

	/* do DOS partitions in the process of getting disklabel? */

	/*
	 * Read dos partition table, follow extended partitions.
	 * Map the partitions to disklabel entries i-p
	 */
	while (wander && loop < DOS_MAXEBR) {
		loop++;
		wander = 0;
		if (part_blkno < extoff)
			part_blkno = extoff;

		/* read MBR/EBR */
		bp->b_blkno = DL_SECTOBLK(lp, part_blkno);
		bp->b_bcount = lp->d_secsize;
		bp->b_error = 0; /* B_ERROR and b_error may have stale data. */
		CLR(bp->b_flags, B_READ | B_WRITE | B_DONE | B_ERROR);
		SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
		(*strat)(bp);
		error = biowait(bp);
		if (error) {
/*wrong*/		if (partoffp)
/*wrong*/			*partoffp = -1;
			return (error);
		}

		bcopy(bp->b_data + DOSPARTOFF, dp, sizeof(dp));

		if (n == 0 && part_blkno == DOSBBSECTOR) {
			u_int16_t mbrtest;

			/* Check the end of sector marker. */
			mbrtest = ((bp->b_data[510] << 8) & 0xff00) |
			    (bp->b_data[511] & 0xff);
			if (mbrtest != 0x55aa)
				goto notmbr;
		}

		if(dp[0].dp_typ == DOSPTYP_EFI)
			return (EINVAL);

		if (ourpart == -1) {
			/* Search for our MBR partition */
			for (dp2=dp, i=0; i < NDOSPART && ourpart == -1;
			    i++, dp2++)
				if (letoh32(dp2->dp_size) &&
				    dp2->dp_typ == DOSPTYP_OPENBSD)
					ourpart = i;
			if (ourpart == -1)
				goto donot;
			/*
			 * This is our MBR partition. need sector
			 * address for SCSI/IDE, cylinder for
			 * ESDI/ST506/RLL
			 */
			dp2 = &dp[ourpart];
			dospartoff = letoh32(dp2->dp_start) + part_blkno;
			dospartend = dospartoff + letoh32(dp2->dp_size);

			/*
			 * Record the OpenBSD partition's placement (in
			 * 512-byte blocks!) for the caller. No need to
			 * finish spoofing.
			 */
			if (partoffp) {
				*partoffp = DL_SECTOBLK(lp, dospartoff);
				return (0);
			}

			if (lp->d_ntracks == 0)
				lp->d_ntracks = dp2->dp_ehd + 1;
			if (lp->d_nsectors == 0)
				lp->d_nsectors = DPSECT(dp2->dp_esect);
			if (lp->d_secpercyl == 0)
				lp->d_secpercyl = lp->d_ntracks *
				    lp->d_nsectors;
		}
donot:
		/*
		 * In case the disklabel read below fails, we want to
		 * provide a fake label in i-p.
		 */
		for (dp2=dp, i=0; i < NDOSPART; i++, dp2++) {
			struct partition *pp;
			u_int8_t fstype;

			if (dp2->dp_typ == DOSPTYP_OPENBSD ||
			    dp2->dp_typ == DOSPTYP_EFI)
				continue;
			if (letoh32(dp2->dp_size) > DL_GETDSIZE(lp))
				continue;
			if (letoh32(dp2->dp_start) > DL_GETDSIZE(lp))
				continue;
			if (letoh32(dp2->dp_size) == 0)
				continue;

			switch (dp2->dp_typ) {
			case DOSPTYP_UNUSED:
				fstype = FS_UNUSED;
				break;

			case DOSPTYP_LINUX:
				fstype = FS_EXT2FS;
				break;

			case DOSPTYP_NTFS:
				fstype = FS_NTFS;
				break;

			case DOSPTYP_EFISYS:
			case DOSPTYP_FAT12:
			case DOSPTYP_FAT16S:
			case DOSPTYP_FAT16B:
			case DOSPTYP_FAT16L:
			case DOSPTYP_FAT32:
			case DOSPTYP_FAT32L:
				fstype = FS_MSDOS;
				break;
			case DOSPTYP_EXTEND:
			case DOSPTYP_EXTENDL:
				part_blkno = letoh32(dp2->dp_start) + extoff;
				if (!extoff) {
					extoff = letoh32(dp2->dp_start);
					part_blkno = 0;
				}
				wander = 1;
				continue;
				break;
			default:
				fstype = FS_OTHER;
				break;
			}

			/*
			 * Don't set fstype/offset/size when just looking for
			 * the offset of the OpenBSD partition. It would
			 * invalidate the disklabel checksum!
			 *
			 * Don't try to spoof more than 8 partitions, i.e.
			 * 'i' -'p'.
			 */
			if (partoffp || n >= 8)
				continue;

			pp = &lp->d_partitions[8+n];
			n++;
			pp->p_fstype = fstype;
			if (letoh32(dp2->dp_start))
				DL_SETPOFFSET(pp,
				    letoh32(dp2->dp_start) + part_blkno);
			DL_SETPSIZE(pp, letoh32(dp2->dp_size));
		}
	}

notmbr:
	if (partoffp == NULL)
		/* Must not modify *lp when partoffp is set. */
		lp->d_npartitions = MAXPARTITIONS;

	if (n == 0 && part_blkno == DOSBBSECTOR && ourpart == -1) {
		u_int16_t fattest;

		/* Check for a valid initial jmp instruction. */
		switch ((u_int8_t)bp->b_data[0]) {
		case 0xeb:
			/*
			 * Two-byte jmp instruction. The 2nd byte is the number
			 * of bytes to jmp and the 3rd byte must be a NOP.
			 */
			if ((u_int8_t)bp->b_data[2] != 0x90)
				goto notfat;
			break;
		case 0xe9:
			/*
			 * Three-byte jmp instruction. The next two bytes are a
			 * little-endian 16 bit value.
			 */
			break;
		default:
			goto notfat;
			break;
		}

		/* Check for a valid bytes per sector value. */
		fattest = ((bp->b_data[12] << 8) & 0xff00) |
		    (bp->b_data[11] & 0xff);
		if (fattest < 512 || fattest > 4096 || (fattest % 512 != 0))
			goto notfat;

		if (partoffp)
			return (ENXIO);	/* No place for disklabel on FAT! */

		DL_SETPSIZE(&lp->d_partitions['i' - 'a'],
		    DL_GETPSIZE(&lp->d_partitions[RAW_PART]));
		DL_SETPOFFSET(&lp->d_partitions['i' - 'a'], 0);
		lp->d_partitions['i' - 'a'].p_fstype = FS_MSDOS;

		spoofonly = 1;	/* No disklabel to read from disk. */
	}

notfat:
	if (n == 0 && part_blkno == DOSBBSECTOR && ourpart == -1) {
		error = readgptlabel(bp, strat, lp, partoffp, spoofonly);
		if (error == 0)
			return (0);
	}

	/* record the OpenBSD partition's placement for the caller */
	if (partoffp)
		*partoffp = DL_SECTOBLK(lp, dospartoff);
	else {
		DL_SETBSTART(lp, dospartoff);
		DL_SETBEND(lp, (dospartend < DL_GETDSIZE(lp)) ? dospartend :
		    DL_GETDSIZE(lp));
	}

	/* don't read the on-disk label if we are in spoofed-only mode */
	if (spoofonly)
		return (0);

	bp->b_blkno = DL_BLKTOSEC(lp, DL_SECTOBLK(lp, dospartoff) +
	    DOS_LABELSECTOR) * DL_BLKSPERSEC(lp);
	offset = DL_BLKOFFSET(lp, DL_SECTOBLK(lp, dospartoff) +
	    DOS_LABELSECTOR);
	bp->b_bcount = lp->d_secsize;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
	(*strat)(bp);
	if (biowait(bp))
		return (bp->b_error);


	error = checkdisklabel(bp->b_data + offset, lp,
	    DL_GETBSTART((struct disklabel*)(bp->b_data+offset)),
	    DL_GETBEND((struct disklabel *)(bp->b_data+offset)));

	return (error);
}

int gpt_get_fstype(struct uuid *);
int gpt_get_hdr(struct buf *, void (*strat)(struct buf *),
    struct disklabel *, u_int64_t, struct gpt_header *);
struct gpt_partition *gpt_get_partition_table(dev_t, void (*)(struct buf *),
    struct disklabel *, struct gpt_header *, u_int64_t *);

int
gpt_get_fstype(struct uuid *uuid_part)
{
	static int init = 0;
	static struct uuid uuid_openbsd, uuid_msdos, uuid_chromerootfs,
	    uuid_chromekernelfs, uuid_linux, uuid_hfs, uuid_unused;
	static const uint8_t gpt_uuid_openbsd[] = GPT_UUID_OPENBSD;
	static const uint8_t gpt_uuid_msdos[] = GPT_UUID_MSDOS;
	static const uint8_t gpt_uuid_chromerootfs[] = GPT_UUID_CHROMEROOTFS;
	static const uint8_t gpt_uuid_chromekernelfs[] = GPT_UUID_CHROMEKERNELFS;
	static const uint8_t gpt_uuid_linux[] = GPT_UUID_LINUX;
	static const uint8_t gpt_uuid_hfs[] = GPT_UUID_APPLE_HFS;
	static const uint8_t gpt_uuid_unused[] = GPT_UUID_UNUSED;

	if (init == 0) {
		uuid_dec_be(gpt_uuid_openbsd, &uuid_openbsd);
		uuid_dec_be(gpt_uuid_msdos, &uuid_msdos);
		uuid_dec_be(gpt_uuid_chromerootfs, &uuid_chromerootfs);
		uuid_dec_be(gpt_uuid_chromekernelfs, &uuid_chromekernelfs);
		uuid_dec_be(gpt_uuid_linux, &uuid_linux);
		uuid_dec_be(gpt_uuid_hfs, &uuid_hfs);
		uuid_dec_be(gpt_uuid_unused, &uuid_unused);
		init = 1;
	}

	if (!memcmp(uuid_part, &uuid_unused, sizeof(struct uuid)))
		return (FS_UNUSED);
	else if (!memcmp(uuid_part, &uuid_openbsd, sizeof(struct uuid)))
		return (FS_BSDFFS);
	else if (!memcmp(uuid_part, &uuid_msdos, sizeof(struct uuid)))
		return (FS_MSDOS);
	else if (!memcmp(uuid_part, &uuid_chromerootfs, sizeof(struct uuid)))
		return (FS_EXT2FS);
	else if (!memcmp(uuid_part, &uuid_chromekernelfs, sizeof(struct uuid)))
		return (FS_MSDOS);
	else if (!memcmp(uuid_part, &uuid_linux, sizeof(struct uuid)))
		return (FS_EXT2FS);
	else if (!memcmp(uuid_part, &uuid_hfs, sizeof(struct uuid)))
		return (FS_HFS);
	else {
		DPRINTF("Unknown GUID: %02x%02x%02x%02x-%02x%02x-%02x%02x-"
		    "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
		    *(((u_int8_t *)uuid_part) + 0),
		    *(((u_int8_t *)uuid_part) + 1),
		    *(((u_int8_t *)uuid_part) + 2),
		    *(((u_int8_t *)uuid_part) + 3),
		    *(((u_int8_t *)uuid_part) + 4),
		    *(((u_int8_t *)uuid_part) + 5),
		    *(((u_int8_t *)uuid_part) + 6),
		    *(((u_int8_t *)uuid_part) + 7),
		    *(((u_int8_t *)uuid_part) + 8),
		    *(((u_int8_t *)uuid_part) + 9),
		    *(((u_int8_t *)uuid_part) + 10),
		    *(((u_int8_t *)uuid_part) + 11),
		    *(((u_int8_t *)uuid_part) + 12),
		    *(((u_int8_t *)uuid_part) + 13),
		    *(((u_int8_t *)uuid_part) + 14),
		    *(((u_int8_t *)uuid_part) + 15)
		    );
		return (FS_OTHER);
	}
}

int
gpt_get_hdr(struct buf *bp, void (*strat)(struct buf *),
    struct disklabel *lp, u_int64_t gptlba, struct gpt_header *gh)
{
	u_int64_t partlastlba;
	int error, partspersec;
	u_int32_t orig_gh_csum, new_gh_csum;

	bp->b_blkno = DL_SECTOBLK(lp, gptlba);
	bp->b_bcount = lp->d_secsize;
	bp->b_error = 0; /* B_ERROR and b_error may have stale data. */
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE | B_ERROR);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);

	DPRINTF("gpt header being read from LBA %llu\n", gptlba);

	(*strat)(bp);

	error = biowait(bp);
	if (error) {
		DPRINTF("error reading gpt from disk (%d)\n", error);
		return (1);
	}
	memcpy(gh, bp->b_data, sizeof(struct gpt_header));

	if (letoh64(gh->gh_sig) != GPTSIGNATURE) {
		DPRINTF("gpt signature: expected 0x%llx, got 0x%llx\n",
		    GPTSIGNATURE, letoh64(gh->gh_sig));
		return (1);
	}

	if (letoh32(gh->gh_rev) != GPTREVISION) {
		DPRINTF("gpt revision: expected 0x%x, got 0x%x\n",
		    GPTREVISION, letoh32(gh->gh_rev));
		return (1);
	}

	if (letoh64(gh->gh_lba_self) != gptlba) {
		DPRINTF("gpt self lba: expected %lld, got %llu\n",
		    gptlba, letoh64(gh->gh_lba_self));
		return (1);
	}

	if (letoh32(gh->gh_size) != GPTMINHDRSIZE) {
		DPRINTF("gpt header size: expected %u, got %u\n",
		    GPTMINHDRSIZE, letoh32(gh->gh_size));
		return (1);
	}

	if (letoh32(gh->gh_part_size) != GPTMINPARTSIZE) {
		DPRINTF("gpt partition entry size: expected %u, got %u\n",
		    GPTMINPARTSIZE, letoh32(gh->gh_part_size));
		return (1);
	}

	if (letoh32(gh->gh_part_num) > GPTPARTITIONS) {
		DPRINTF("gpt partition count: expected <= %u, got %u\n",
		    GPTPARTITIONS, letoh32(gh->gh_part_num));
		return (1);
	}

	orig_gh_csum = gh->gh_csum;
	gh->gh_csum = 0;
	new_gh_csum = crc32(0, (unsigned char *)gh, letoh32(gh->gh_size));
	gh->gh_csum = orig_gh_csum;
	if (letoh32(orig_gh_csum) != new_gh_csum) {
		DPRINTF("gpt header checksum: expected 0x%x, got 0x%x\n",
		    new_gh_csum, orig_gh_csum);
		return (1);
	}

	if (letoh64(gh->gh_lba_end) >= DL_GETDSIZE(lp)) {
		DPRINTF("gpt last usable LBA: expected < %lld, got %llu\n",
		    DL_GETDSIZE(lp), letoh64(gh->gh_lba_end));
		return (1);
	}

	if (letoh64(gh->gh_lba_start) >= letoh64(gh->gh_lba_end)) {
		DPRINTF("gpt first usable LBA: expected < %llu, got %llu\n",
		    letoh64(gh->gh_lba_end), letoh64(gh->gh_lba_start));
		return (1);
	}

	if (letoh64(gh->gh_part_lba) <= letoh64(gh->gh_lba_end) &&
	    letoh64(gh->gh_part_lba) >= letoh64(gh->gh_lba_start)) {
		DPRINTF("gpt partition table start LBA: expected < %llu or "
		    "> %llu, got %llu\n", letoh64(gh->gh_lba_start),
		    letoh64(gh->gh_lba_end), letoh64(gh->gh_part_lba));
		return (1);
	}

	partspersec = lp->d_secsize / letoh32(gh->gh_part_size);
	partlastlba = letoh64(gh->gh_part_lba) +
	    ((letoh32(gh->gh_part_num) + partspersec - 1) / partspersec) - 1;
	if (partlastlba <= letoh64(gh->gh_lba_end) &&
	    partlastlba >= letoh64(gh->gh_lba_start)) {
		DPRINTF("gpt partition table last LBA: expected < %llu or "
		    "> %llu, got %llu\n", letoh64(gh->gh_lba_start),
		    letoh64(gh->gh_lba_end), partlastlba);
		return (1);
	}

	/*
	 * Other possible paranoia checks:
	 *	1) partition table starts before primary gpt lba.
	 *	2) partition table extends into lowest partition.
	 *	3) alt partition table starts before gh_lba_end.
	 */
	return (0);
}

struct gpt_partition *
gpt_get_partition_table(dev_t dev, void (*strat)(struct buf *),
    struct disklabel *lp, struct gpt_header *gh, u_int64_t *gpsz)
{
	struct buf *bp;
	unsigned char *gp;
	int error, secs;
	uint32_t partspersec;
	uint32_t checksum;

	partspersec = lp->d_secsize / letoh32(gh->gh_part_size);
	if (partspersec * letoh32(gh->gh_part_size) != lp->d_secsize) {
		DPRINTF("gpt partition table entry invalid size %u.\n",
		    letoh32(gh->gh_part_size));
		return (NULL);
	}
	secs = (letoh32(gh->gh_part_num) + partspersec - 1) / partspersec;

	gp = mallocarray(secs, lp->d_secsize, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (gp == NULL) {
		DPRINTF("gpt partition table can't be allocated\n");
		return (NULL);
	}
	*gpsz = secs * lp->d_secsize;

	bp = geteblk((int)*gpsz);
	bp->b_dev = dev;

	bp->b_blkno = DL_SECTOBLK(lp, letoh64(gh->gh_part_lba));
	bp->b_bcount = *gpsz;
	/* B_ERROR and b_error may have stale data. */
	bp->b_error = 0;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE | B_ERROR);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);

	DPRINTF("gpt partition table being read from LBA %llu\n",
	    letoh64(gh->gh_part_lba));

	(*strat)(bp);

	error = biowait(bp);
	if (error) {
		DPRINTF("gpt partition table read error %d\n", error);
		free(gp, M_DEVBUF, *gpsz);
		gp = NULL;
		goto done;
	}

	checksum = crc32(0, bp->b_data, letoh32(gh->gh_part_num) *
	    letoh32(gh->gh_part_size));
	if (checksum == letoh32(gh->gh_part_csum))
		memcpy(gp, bp->b_data, *gpsz);
	else {
		DPRINTF("gpt partition table checksum: expected %x, got %x\n",
		    checksum, letoh32(gh->gh_part_csum));
		free(gp, M_DEVBUF, *gpsz);
		gp = NULL;
	}

done:
	bp->b_flags |= B_INVAL;
	brelse(bp);

	return ((struct gpt_partition *)gp);
}

/*
 * If gpt partition table requested, attempt to load it and
 * find disklabel inside a GPT partition. Return buffer
 * for use in signalling errors if requested.
 *
 * XXX: readgptlabel() is based on readdoslabel(), so they should be merged
 */
int
readgptlabel(struct buf *bp, void (*strat)(struct buf *), struct disklabel *lp,
    daddr_t *partoffp, int spoofonly)
{
	struct gpt_header gh, altgh;
	struct gpt_partition *gp, *gp_tmp;
	struct uuid uuid_part, uuid_openbsd;
	struct partition *pp;
	static const u_int8_t gpt_uuid_openbsd[] = GPT_UUID_OPENBSD;
	u_int64_t gpsz;
	u_int64_t gptpartoff = 0, gptpartend = DL_GETBEND(lp);
	int i, error, n=0, ourpart = -1, offset, ghinvalid, altghinvalid;
	u_int8_t fstype;

	uuid_dec_be(gpt_uuid_openbsd, &uuid_openbsd);

	if (lp->d_secpercyl == 0)
		return (EINVAL);	/* invalid label */
	if (lp->d_secsize == 0)
		return (ENOSPC);	/* disk too small */

	ghinvalid = gpt_get_hdr(bp, strat, lp, GPTSECTOR, &gh);
	altghinvalid = gpt_get_hdr(bp, strat, lp, DL_GETDSIZE(lp) - 1, &altgh);
	if (ghinvalid && altghinvalid)
		return(EINVAL);
	if (ghinvalid)
		gh = altgh;

	gp = gpt_get_partition_table(bp->b_dev, strat, lp, &gh, &gpsz);
	if (gp == NULL)
		return (EINVAL);

	/* find OpenBSD partition */
	for (gp_tmp = gp, i = 0; i < letoh32(gh.gh_part_num) && ourpart == -1;
	    gp_tmp++, i++) {
		if (letoh64(gp_tmp->gp_lba_start) > letoh64(gp_tmp->gp_lba_end)
		    || letoh64(gp_tmp->gp_lba_start) < letoh64(gh.gh_lba_start)
		    || letoh64(gp_tmp->gp_lba_end) > letoh64(gh.gh_lba_end))
			continue; /* entry invalid */

		uuid_dec_le(&gp_tmp->gp_type, &uuid_part);
		if (!memcmp(&uuid_part, &uuid_openbsd, sizeof(struct uuid))) {
			ourpart = i; /* found it */
		}

		/*
		 * In case the disklabel read below fails, we want to
		 * provide a fake label in i-p.
		 */
		fstype = gpt_get_fstype(&uuid_part);

		/*
		 * Don't set fstype/offset/size when just looking for
		 * the offset of the OpenBSD partition. It would
		 * invalidate the disklabel checksum!
		 *
		 * Don't try to spoof more than 8 partitions, i.e.
		 * 'i' -'p'.
		 */
		if (partoffp || n >= 8)
			continue;

		pp = &lp->d_partitions[8+n];
		n++;
		pp->p_fstype = fstype;
		DL_SETPOFFSET(pp, letoh64(gp_tmp->gp_lba_start));
		DL_SETPSIZE(pp, letoh64(gp_tmp->gp_lba_end)
		    - letoh64(gp_tmp->gp_lba_start) + 1);
	}

	if (ourpart != -1) {
		/* found our OpenBSD partition, so use it */
		gp_tmp = &gp[ourpart];
		gptpartoff = letoh64(gp_tmp->gp_lba_start);
		gptpartend = letoh64(gp_tmp->gp_lba_end) + 1;
	} else
		spoofonly = 1;	/* No disklabel to read from disk. */

	free(gp, M_DEVBUF, gpsz);

	if (!partoffp)
		/* Must not modify *lp when partoffp is set. */
		lp->d_npartitions = MAXPARTITIONS;


	/* record the OpenBSD partition's placement for the caller */
	if (partoffp)
		*partoffp = DL_SECTOBLK(lp, gptpartoff);
	else {
		DL_SETBSTART(lp, gptpartoff);
		DL_SETBEND(lp, (gptpartend < DL_GETDSIZE(lp)) ? gptpartend :
		    DL_GETDSIZE(lp));
	}

	/* don't read the on-disk label if we are in spoofed-only mode */
	if (spoofonly)
		return (0);

	bp->b_blkno = DL_BLKTOSEC(lp, DL_SECTOBLK(lp, gptpartoff) +
	    DOS_LABELSECTOR) * DL_BLKSPERSEC(lp);
	offset = DL_BLKOFFSET(lp, DL_SECTOBLK(lp, gptpartoff) +
	    DOS_LABELSECTOR);
	bp->b_bcount = lp->d_secsize;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
	(*strat)(bp);
	if (biowait(bp))
		return (bp->b_error);

	/* sub-GPT disklabels are always at a LABELOFFSET of 0 */
	error = checkdisklabel(bp->b_data + offset, lp,
	    DL_GETBSTART((struct disklabel*)(bp->b_data+offset)),
	    DL_GETBEND((struct disklabel *)(bp->b_data+offset)));

	return (error);
}

/*
 * Check new disk label for sensibility before setting it.
 */
int
setdisklabel(struct disklabel *olp, struct disklabel *nlp, u_int openmask)
{
	struct partition *opp, *npp;
	struct disk *dk;
	u_int64_t uid;
	int i;

	/* sanity clause */
	if (nlp->d_secpercyl == 0 || nlp->d_secsize == 0 ||
	    (nlp->d_secsize % DEV_BSIZE) != 0)
		return (EINVAL);

	/* special case to allow disklabel to be invalidated */
	if (nlp->d_magic == 0xffffffff) {
		*olp = *nlp;
		return (0);
	}

	if (nlp->d_magic != DISKMAGIC || nlp->d_magic2 != DISKMAGIC ||
	    dkcksum(nlp) != 0)
		return (EINVAL);

	/* XXX missing check if other dos partitions will be overwritten */

	for (i = 0; i < MAXPARTITIONS; i++) {
		opp = &olp->d_partitions[i];
		npp = &nlp->d_partitions[i];
		if ((openmask & (1 << i)) &&
		    (DL_GETPOFFSET(npp) != DL_GETPOFFSET(opp) ||
		    DL_GETPSIZE(npp) < DL_GETPSIZE(opp)))
			return (EBUSY);
		/*
		 * Copy internally-set partition information
		 * if new label doesn't include it.		XXX
		 */
		if (npp->p_fstype == FS_UNUSED && opp->p_fstype != FS_UNUSED) {
			npp->p_fragblock = opp->p_fragblock;
			npp->p_cpg = opp->p_cpg;
		}
	}

	/* Generate a UID if the disklabel does not already have one. */
	uid = 0;
	if (memcmp(nlp->d_uid, &uid, sizeof(nlp->d_uid)) == 0) {
		do {
			arc4random_buf(nlp->d_uid, sizeof(nlp->d_uid));
			TAILQ_FOREACH(dk, &disklist, dk_link)
				if (dk->dk_label && memcmp(dk->dk_label->d_uid,
				    nlp->d_uid, sizeof(nlp->d_uid)) == 0)
					break;
		} while (dk != NULL &&
		    memcmp(nlp->d_uid, &uid, sizeof(nlp->d_uid)) == 0);
	}

	nlp->d_checksum = 0;
	nlp->d_checksum = dkcksum(nlp);
	*olp = *nlp;

	disk_change = 1;

	return (0);
}

/*
 * Determine the size of the transfer, and make sure it is within the
 * boundaries of the partition. Adjust transfer if needed, and signal errors or
 * early completion.
 */
int
bounds_check_with_label(struct buf *bp, struct disklabel *lp)
{
	struct partition *p = &lp->d_partitions[DISKPART(bp->b_dev)];
	daddr_t partblocks, sz;

	/* Avoid division by zero, negative offsets, and negative sizes. */
	if (lp->d_secpercyl == 0 || bp->b_blkno < 0 || bp->b_bcount < 0)
		goto bad;

	/* Ensure transfer is a whole number of aligned sectors. */
	if ((bp->b_blkno % DL_BLKSPERSEC(lp)) != 0 ||
	    (bp->b_bcount % lp->d_secsize) != 0)
		goto bad;

	/* Ensure transfer starts within partition boundary. */
	partblocks = DL_SECTOBLK(lp, DL_GETPSIZE(p));
	if (bp->b_blkno > partblocks)
		goto bad;

	/* If exactly at end of partition or null transfer, return EOF. */
	if (bp->b_blkno == partblocks || bp->b_bcount == 0)
		goto done;

	/* Truncate request if it exceeds past the end of the partition. */
	sz = bp->b_bcount >> DEV_BSHIFT;
	if (sz > partblocks - bp->b_blkno) {
		sz = partblocks - bp->b_blkno;
		bp->b_bcount = sz << DEV_BSHIFT;
	}

	return (0);

 bad:
	bp->b_error = EINVAL;
	bp->b_flags |= B_ERROR;
 done:
	bp->b_resid = bp->b_bcount;
	return (-1);
}

/*
 * Disk error is the preface to plaintive error messages
 * about failing disk transfers.  It prints messages of the form

hp0g: hard error reading fsbn 12345 of 12344-12347 (hp0 bn %d cn %d tn %d sn %d)

 * if the offset of the error in the transfer and a disk label
 * are both available.  blkdone should be -1 if the position of the error
 * is unknown; the disklabel pointer may be null from drivers that have not
 * been converted to use them.  The message is printed with printf
 * if pri is LOG_PRINTF, otherwise it uses log at the specified priority.
 * The message should be completed (with at least a newline) with printf
 * or addlog, respectively.  There is no trailing space.
 */
void
diskerr(struct buf *bp, char *dname, char *what, int pri, int blkdone,
    struct disklabel *lp)
{
	int unit = DISKUNIT(bp->b_dev), part = DISKPART(bp->b_dev);
	int (*pr)(const char *, ...) __attribute__((__format__(__kprintf__,1,2)));
	char partname = 'a' + part;
	daddr_t sn;

	if (pri != LOG_PRINTF) {
		log(pri, "%s", "");
		pr = addlog;
	} else
		pr = printf;
	(*pr)("%s%d%c: %s %sing fsbn ", dname, unit, partname, what,
	    bp->b_flags & B_READ ? "read" : "writ");
	sn = bp->b_blkno;
	if (bp->b_bcount <= DEV_BSIZE)
		(*pr)("%lld", (long long)sn);
	else {
		if (blkdone >= 0) {
			sn += blkdone;
			(*pr)("%lld of ", (long long)sn);
		}
		(*pr)("%lld-%lld", (long long)bp->b_blkno,
		    (long long)(bp->b_blkno + (bp->b_bcount - 1) / DEV_BSIZE));
	}
	if (lp && (blkdone >= 0 || bp->b_bcount <= lp->d_secsize)) {
		sn += DL_SECTOBLK(lp, DL_GETPOFFSET(&lp->d_partitions[part]));
		(*pr)(" (%s%d bn %lld; cn %lld", dname, unit, (long long)sn,
		    (long long)(sn / DL_SECTOBLK(lp, lp->d_secpercyl)));
		sn %= DL_SECTOBLK(lp, lp->d_secpercyl);
		(*pr)(" tn %lld sn %lld)",
		    (long long)(sn / DL_SECTOBLK(lp, lp->d_nsectors)),
		    (long long)(sn % DL_SECTOBLK(lp, lp->d_nsectors)));
	}
}

/*
 * Initialize the disklist.  Called by main() before autoconfiguration.
 */
void
disk_init(void)
{

	TAILQ_INIT(&disklist);
	disk_count = disk_change = 0;
}

int
disk_construct(struct disk *diskp)
{
	rw_init(&diskp->dk_lock, "dklk");
	mtx_init(&diskp->dk_mtx, IPL_BIO);

	diskp->dk_flags |= DKF_CONSTRUCTED;

	return (0);
}

/*
 * Attach a disk.
 */
void
disk_attach(struct device *dv, struct disk *diskp)
{
	int majdev;

	if (!ISSET(diskp->dk_flags, DKF_CONSTRUCTED))
		disk_construct(diskp);

	/*
	 * Allocate and initialize the disklabel structures.  Note that
	 * it's not safe to sleep here, since we're probably going to be
	 * called during autoconfiguration.
	 */
	diskp->dk_label = malloc(sizeof(struct disklabel), M_DEVBUF,
	    M_NOWAIT|M_ZERO);
	if (diskp->dk_label == NULL)
		panic("disk_attach: can't allocate storage for disklabel");

	/*
	 * Set the attached timestamp.
	 */
	microuptime(&diskp->dk_attachtime);

	/*
	 * Link into the disklist.
	 */
	TAILQ_INSERT_TAIL(&disklist, diskp, dk_link);
	++disk_count;
	disk_change = 1;

	/*
	 * Store device structure and number for later use.
	 */
	diskp->dk_device = dv;
	diskp->dk_devno = NODEV;
	if (dv != NULL) {
		majdev = findblkmajor(dv);
		if (majdev >= 0)
			diskp->dk_devno =
			    MAKEDISKDEV(majdev, dv->dv_unit, RAW_PART);

		if (diskp->dk_devno != NODEV) {
			struct disk_attach_task *dat;

			dat = malloc(sizeof(*dat), M_TEMP, M_WAITOK);

			/* XXX: Assumes dk is part of the device softc. */
			device_ref(dv);
			dat->dk = diskp;

			task_set(&dat->task, disk_attach_callback, dat);
			task_add(systq, &dat->task);
		}
	}

	if (softraid_disk_attach)
		softraid_disk_attach(diskp, 1);
}

void
disk_attach_callback(void *xdat)
{
	struct disk_attach_task *dat = xdat;
	struct disk *dk = dat->dk;
	char errbuf[100];
	struct disklabel dl;
	dev_t dev;

	free(dat, M_TEMP, sizeof(*dat));

	if (dk->dk_flags & (DKF_OPENED | DKF_NOLABELREAD))
		goto done;

	/* Read disklabel. */
	dev = dk->dk_devno;
	if (disk_readlabel(&dl, dk->dk_devno, errbuf, sizeof(errbuf)) == NULL) {
		add_timer_randomness(dl.d_checksum);
		dk->dk_flags |= DKF_LABELVALID;
	}

done:
	dk->dk_flags |= DKF_OPENED;
	device_unref(dk->dk_device);
	wakeup(dk);
}

/*
 * Detach a disk.
 */
void
disk_detach(struct disk *diskp)
{

	if (softraid_disk_attach)
		softraid_disk_attach(diskp, -1);

	/*
	 * Free the space used by the disklabel structures.
	 */
	free(diskp->dk_label, M_DEVBUF, sizeof(*diskp->dk_label));
	diskp->dk_flags &= ~(DKF_OPENED | DKF_LABELVALID);

	/*
	 * Remove from the disklist.
	 */
	TAILQ_REMOVE(&disklist, diskp, dk_link);
	disk_change = 1;
	if (--disk_count < 0)
		panic("disk_detach: disk_count < 0");
}

int
disk_openpart(struct disk *dk, int part, int fmt, int haslabel)
{
	KASSERT(part >= 0 && part < MAXPARTITIONS);

	/* Unless opening the raw partition, check that the partition exists. */
	if (part != RAW_PART && (!haslabel ||
	    part >= dk->dk_label->d_npartitions ||
	    dk->dk_label->d_partitions[part].p_fstype == FS_UNUSED))
		return (ENXIO);

	/* Ensure the partition doesn't get changed under our feet. */
	switch (fmt) {
	case S_IFCHR:
		dk->dk_copenmask |= (1 << part);
		break;
	case S_IFBLK:
		dk->dk_bopenmask |= (1 << part);
		break;
	}
	dk->dk_openmask = dk->dk_copenmask | dk->dk_bopenmask;

	return (0);
}

void
disk_closepart(struct disk *dk, int part, int fmt)
{
	KASSERT(part >= 0 && part < MAXPARTITIONS);

	switch (fmt) {
	case S_IFCHR:
		dk->dk_copenmask &= ~(1 << part);
		break;
	case S_IFBLK:
		dk->dk_bopenmask &= ~(1 << part);
		break;
	}
	dk->dk_openmask = dk->dk_copenmask | dk->dk_bopenmask;
}

void
disk_gone(struct disk *diskp)
{
	int bmaj, cmaj, mn;

	/* Locate the lowest minor number to be detached. */
	mn = DISKMINOR(DISKUNIT(diskp->dk_devno), 0);

	bmaj = major(diskp->dk_devno);
	cmaj = major(blktochr(diskp->dk_devno));

	vdevgone(bmaj, mn, mn + MAXPARTITIONS - 1, VBLK);
	vdevgone(cmaj, mn, mn + MAXPARTITIONS - 1, VCHR);
}

/*
 * Increment a disk's busy counter.  If the counter is going from
 * 0 to 1, set the timestamp.
 */
void
disk_busy(struct disk *diskp)
{

	/*
	 * XXX We'd like to use something as accurate as microtime(),
	 * but that doesn't depend on the system TOD clock.
	 */
	mtx_enter(&diskp->dk_mtx);
	if (diskp->dk_busy++ == 0)
		microuptime(&diskp->dk_timestamp);
	mtx_leave(&diskp->dk_mtx);
}

/*
 * Decrement a disk's busy counter, increment the byte count, total busy
 * time, and reset the timestamp.
 */
void
disk_unbusy(struct disk *diskp, long bcount, int read)
{
	struct timeval dv_time, diff_time;

	mtx_enter(&diskp->dk_mtx);

	if (diskp->dk_busy-- == 0)
		printf("disk_unbusy: %s: dk_busy < 0\n", diskp->dk_name);

	microuptime(&dv_time);

	timersub(&dv_time, &diskp->dk_timestamp, &diff_time);
	timeradd(&diskp->dk_time, &diff_time, &diskp->dk_time);

	diskp->dk_timestamp = dv_time;
	if (bcount > 0) {
		if (read) {
			diskp->dk_rbytes += bcount;
			diskp->dk_rxfer++;
		} else {
			diskp->dk_wbytes += bcount;
			diskp->dk_wxfer++;
		}
	} else
		diskp->dk_seek++;

	mtx_leave(&diskp->dk_mtx);

	add_disk_randomness(bcount ^ diff_time.tv_usec);
}

int
disk_lock(struct disk *dk)
{
	return (rw_enter(&dk->dk_lock, RW_WRITE|RW_INTR));
}

void
disk_lock_nointr(struct disk *dk)
{
	rw_enter_write(&dk->dk_lock);
}

void
disk_unlock(struct disk *dk)
{
	rw_exit_write(&dk->dk_lock);
}

int
dk_mountroot(void)
{
	char errbuf[100];
	int part = DISKPART(rootdev);
	int (*mountrootfn)(void);
	struct disklabel dl;
	char *error;

	error = disk_readlabel(&dl, rootdev, errbuf, sizeof(errbuf));
	if (error)
		panic("%s", error);

	if (DL_GETPSIZE(&dl.d_partitions[part]) == 0)
		panic("root filesystem has size 0");
	switch (dl.d_partitions[part].p_fstype) {
#ifdef EXT2FS
	case FS_EXT2FS:
		{
		extern int ext2fs_mountroot(void);
		mountrootfn = ext2fs_mountroot;
		}
		break;
#endif
#ifdef FFS
	case FS_BSDFFS:
		{
		extern int ffs_mountroot(void);
		mountrootfn = ffs_mountroot;
		}
		break;
#endif
#ifdef CD9660
	case FS_ISO9660:
		{
		extern int cd9660_mountroot(void);
		mountrootfn = cd9660_mountroot;
		}
		break;
#endif
#ifdef TMPFS
	case FS_TMPFS:
		{
		extern int tmpfs_mountroot(void);
		mountrootfn = tmpfs_mountroot;
		break;
		}
#endif
	default:
#ifdef FFS
		{
		extern int ffs_mountroot(void);

		printf("filesystem type %d not known.. assuming ffs\n",
		    dl.d_partitions[part].p_fstype);
		mountrootfn = ffs_mountroot;
		}
#else
		panic("disk 0x%x filesystem type %d not known",
		    rootdev, dl.d_partitions[part].p_fstype);
#endif
	}
	return (*mountrootfn)();
}

struct device *
getdisk(char *str, size_t len, int defpart, dev_t *devp)
{
	struct device *dv;

	if ((dv = parsedisk(str, len, defpart, devp)) == NULL) {
		printf("use one of: exit");
		TAILQ_FOREACH(dv, &alldevs, dv_list) {
			if (dv->dv_class == DV_DISK)
				printf(" %s[a-p]", dv->dv_xname);
#if defined(NFSCLIENT)
			if (dv->dv_class == DV_IFNET)
				printf(" %s", dv->dv_xname);
#endif
		}
		printf("\n");
	}
	return (dv);
}

struct device *
parsedisk(char *str, size_t len, int defpart, dev_t *devp)
{
	struct device *dv;
	int majdev, part = defpart;
	char c;

	if (len == 0)
		return (NULL);
	c = str[len-1];
	if (c >= 'a' && (c - 'a') < MAXPARTITIONS) {
		part = c - 'a';
		len -= 1;
	}

	TAILQ_FOREACH(dv, &alldevs, dv_list) {
		if (dv->dv_class == DV_DISK &&
		    strncmp(str, dv->dv_xname, len) == 0 &&
		    dv->dv_xname[len] == '\0') {
			majdev = findblkmajor(dv);
			if (majdev < 0)
				return NULL;
			*devp = MAKEDISKDEV(majdev, dv->dv_unit, part);
			break;
		}
#if defined(NFSCLIENT)
		if (dv->dv_class == DV_IFNET &&
		    strncmp(str, dv->dv_xname, len) == 0 &&
		    dv->dv_xname[len] == '\0') {
			*devp = NODEV;
			break;
		}
#endif
	}

	return (dv);
}

struct device *
setroot_swapgeneric(struct device *bootdv, dev_t *nrootdev)
{
	struct device *rootdv;
	struct disk *dk;
	u_char duid[8];

	if (tmpfsrd_present() == true) {
		/* give tmpfsrd preference if configured. */
		char *tmpfsdisk = "tmpfsrd0a";
		rootdv = parsedisk(tmpfsdisk, strlen(tmpfsdisk), 0, nrootdev);
		if (rootdv == NULL)
			panic("tmpfsrd configured, but not attached");
		return (rootdv);
	} else {
		/* resort to old behaviour of root on bootdev */
		if (bootdv->dv_class != DV_DISK)
			return (bootdv);
		memset(&duid, 0, sizeof(duid));
		if (memcmp(rootduid, &duid, sizeof(rootduid)) != 0) {
			TAILQ_FOREACH(dk, &disklist, dk_link) {
				if ((dk->dk_flags & DKF_LABELVALID) &&
				    dk->dk_label && memcmp(dk->dk_label->d_uid,
				    &rootduid, sizeof(rootduid)) == 0)
					break;
			}
			if (dk == NULL)
				panic("root device (%02hx%02hx%02hx%02hx%02hx"
				    "%02hx%02hx%02hx) not found", rootduid[0],
				    rootduid[1], rootduid[2], rootduid[3],
				    rootduid[4], rootduid[5], rootduid[6],
				    rootduid[7]);
			return (dk->dk_device);
		}
		return (bootdv);
	}
}

void
setroot(struct device *bootdv, int part, int exitflags)
{
	int majdev, unit, s, slept = 0;
	size_t len;
	struct swdevt *swp;
	struct device *rootdv, *dv;
	dev_t nrootdev, nswapdev = NODEV, temp = NODEV;
	struct ifnet *ifp = NULL;
	struct disk *dk;
	u_char duid[8];
	char buf[128];
#if defined(NFSCLIENT)
	extern char *nfsbootdevname;
#endif

	/* Ensure that all disk attach callbacks have completed. */
	do {
		TAILQ_FOREACH(dk, &disklist, dk_link) {
			if (dk->dk_devno != NODEV &&
			    (dk->dk_flags & DKF_OPENED) == 0) {
				tsleep(dk, 0, "dkopen", hz);
				slept++;
				break;
			}
		}
	} while (dk != NULL && slept < 5);

	if (slept == 5) {
		printf("disklabels not read:");
		TAILQ_FOREACH(dk, &disklist, dk_link)
			if (dk->dk_devno != NODEV &&
			    (dk->dk_flags & DKF_OPENED) == 0)
				printf(" %s", dk->dk_name);
		printf("\n");
	}

	/* Locate DUID for boot disk if not already provided. */
	memset(duid, 0, sizeof(duid));
	if (memcmp(bootduid, duid, sizeof(bootduid)) == 0) {
		TAILQ_FOREACH(dk, &disklist, dk_link)
			if (dk->dk_device == bootdv)
				break;
		if (dk && (dk->dk_flags & DKF_LABELVALID))
			bcopy(dk->dk_label->d_uid, bootduid, sizeof(bootduid));
	}
	bcopy(bootduid, rootduid, sizeof(rootduid));

#if NSOFTRAID > 0
	sr_map_root();
#endif

	/*
	 * If `swap generic' and we couldn't determine boot device,
	 * ask the user.
	 */
	dk = NULL;
	if (mountroot == NULL && bootdv == NULL)
		boothowto |= RB_ASKNAME;
	if (boothowto & RB_ASKNAME) {
		while (1) {
			printf("root device");
			if (bootdv != NULL) {
				printf(" (default %s", bootdv->dv_xname);
				if (bootdv->dv_class == DV_DISK)
					printf("%c", 'a' + part);
				printf(")");
			}
			printf(": ");
			s = splhigh();
			cnpollc(1);
			len = getsn(buf, sizeof(buf));
			cnpollc(0);
			splx(s);
			if (strcmp(buf, "exit") == 0)
				reboot(exitflags);
			if (len == 0 && bootdv != NULL) {
				strlcpy(buf, bootdv->dv_xname, sizeof buf);
				len = strlen(buf);
			}
			if (len > 0 && buf[len - 1] == '*') {
				buf[--len] = '\0';
				dv = getdisk(buf, len, part, &nrootdev);
				if (dv != NULL) {
					rootdv = dv;
					nswapdev = nrootdev;
					goto gotswap;
				}
			}
			dv = getdisk(buf, len, part, &nrootdev);
			if (dv != NULL) {
				rootdv = dv;
				break;
			}
		}

		if (rootdv->dv_class == DV_IFNET)
			goto gotswap;

		/* try to build swap device out of new root device */
		while (1) {
			printf("swap device");
			if (rootdv != NULL)
				printf(" (default %s%s)", rootdv->dv_xname,
				    rootdv->dv_class == DV_DISK ? "b" : "");
			printf(": ");
			s = splhigh();
			cnpollc(1);
			len = getsn(buf, sizeof(buf));
			cnpollc(0);
			splx(s);
			if (strcmp(buf, "exit") == 0)
				reboot(exitflags);
			if (len == 0 && rootdv != NULL) {
				switch (rootdv->dv_class) {
				case DV_IFNET:
					nswapdev = NODEV;
					break;
				case DV_DISK:
					nswapdev = MAKEDISKDEV(major(nrootdev),
					    DISKUNIT(nrootdev), 1);
					if (nswapdev == nrootdev)
						continue;
					break;
				default:
					break;
				}
				break;
			}
			dv = getdisk(buf, len, 1, &nswapdev);
			if (dv) {
				if (dv->dv_class == DV_IFNET)
					nswapdev = NODEV;
				if (nswapdev == nrootdev)
					continue;
				break;
			}
		}
gotswap:
		rootdev = nrootdev;
		dumpdev = nswapdev;
		swdevt[0].sw_dev = nswapdev;
		swdevt[1].sw_dev = NODEV;
#if defined(NFSCLIENT)
	} else if (mountroot == nfs_mountroot) {
		rootdv = bootdv;
		rootdev = dumpdev = swapdev = NODEV;
#endif
	} else if (mountroot == NULL && rootdev == NODEV) {
		/*
		 * `swap generic'
		 */
		rootdv = setroot_swapgeneric(bootdv, &nrootdev);
		majdev = findblkmajor(rootdv);
		if (majdev >= 0) {
			/*
			 * Root and swap are on the disk.
			 * Assume swap is on partition b.
			 */
			rootdev = MAKEDISKDEV(majdev, rootdv->dv_unit, part);
			nswapdev = MAKEDISKDEV(majdev, rootdv->dv_unit, 1);
		} else {
			/*
			 * Root and swap are on a net.
			 */
			nswapdev = NODEV;
		}
		dumpdev = nswapdev;
		swdevt[0].sw_dev = nswapdev;
		/* swdevt[1].sw_dev = NODEV; */
	} else {
		/* Completely pre-configured, but we want rootdv .. */
		majdev = major(rootdev);
		if (findblkname(majdev) == NULL)
			return;
		unit = DISKUNIT(rootdev);
		part = DISKPART(rootdev);
		snprintf(buf, sizeof buf, "%s%d%c",
		    findblkname(majdev), unit, 'a' + part);
		rootdv = parsedisk(buf, strlen(buf), 0, &nrootdev);
		if (rootdv == NULL)
			panic("root device (%s) not found", buf);
	}

	if (rootdv && rootdv == bootdv && rootdv->dv_class == DV_IFNET)
		ifp = ifunit(rootdv->dv_xname);
	else if (bootdv && bootdv->dv_class == DV_IFNET)
		ifp = ifunit(bootdv->dv_xname);

	if (ifp)
		if_addgroup(ifp, "netboot");

	switch (rootdv->dv_class) {
#if defined(NFSCLIENT)
	case DV_IFNET:
		mountroot = nfs_mountroot;
		nfsbootdevname = rootdv->dv_xname;
		return;
#endif
	case DV_DISK:
		mountroot = dk_mountroot;
		part = DISKPART(rootdev);
		break;
	default:
		printf("can't figure root, hope your kernel is right\n");
		return;
	}

	printf("root on %s%c", rootdv->dv_xname, 'a' + part);

	if (dk && dk->dk_device == rootdv)
		printf(" (%02hx%02hx%02hx%02hx%02hx%02hx%02hx%02hx.%c)",
		    rootduid[0], rootduid[1], rootduid[2], rootduid[3],
		    rootduid[4], rootduid[5], rootduid[6], rootduid[7],
		    'a' + part);

	/*
	 * Make the swap partition on the root drive the primary swap.
	 */
	for (swp = swdevt; swp->sw_dev != NODEV; swp++) {
		if (major(rootdev) == major(swp->sw_dev) &&
		    DISKUNIT(rootdev) == DISKUNIT(swp->sw_dev)) {
			temp = swdevt[0].sw_dev;
			swdevt[0].sw_dev = swp->sw_dev;
			swp->sw_dev = temp;
			break;
		}
	}
	if (swp->sw_dev != NODEV) {
		/*
		 * If dumpdev was the same as the old primary swap device,
		 * move it to the new primary swap device.
		 */
		if (temp == dumpdev)
			dumpdev = swdevt[0].sw_dev;
	}
	if (swdevt[0].sw_dev != NODEV)
		printf(" swap on %s%d%c", findblkname(major(swdevt[0].sw_dev)),
		    DISKUNIT(swdevt[0].sw_dev),
		    'a' + DISKPART(swdevt[0].sw_dev));
	if (dumpdev != NODEV)
		printf(" dump on %s%d%c", findblkname(major(dumpdev)),
		    DISKUNIT(dumpdev), 'a' + DISKPART(dumpdev));
	printf("\n");
}

extern struct nam2blk nam2blk[];

int
findblkmajor(struct device *dv)
{
	char buf[16], *p;
	int i;

	if (strlcpy(buf, dv->dv_xname, sizeof buf) >= sizeof buf)
		return (-1);
	for (p = buf; *p; p++)
		if (*p >= '0' && *p <= '9')
			*p = '\0';

	for (i = 0; nam2blk[i].name; i++)
		if (!strcmp(buf, nam2blk[i].name))
			return (nam2blk[i].maj);
	return (-1);
}

char *
findblkname(int maj)
{
	int i;

	for (i = 0; nam2blk[i].name; i++)
		if (nam2blk[i].maj == maj)
			return (nam2blk[i].name);
	return (NULL);
}

char *
disk_readlabel(struct disklabel *dl, dev_t dev, char *errbuf, size_t errsize)
{
	struct vnode *vn;
	dev_t chrdev, rawdev;
	int error;

	chrdev = blktochr(dev);
	rawdev = MAKEDISKDEV(major(chrdev), DISKUNIT(chrdev), RAW_PART);

#ifdef DEBUG
	printf("dev=0x%x chrdev=0x%x rawdev=0x%x\n", dev, chrdev, rawdev);
#endif

	if (cdevvp(rawdev, &vn)) {
		snprintf(errbuf, errsize,
		    "cannot obtain vnode for 0x%x/0x%x", dev, rawdev);
		return (errbuf);
	}

	error = VOP_OPEN(vn, FREAD, NOCRED);
	if (error) {
		snprintf(errbuf, errsize,
		    "cannot open disk, 0x%x/0x%x, error %d",
		    dev, rawdev, error);
		goto done;
	}

	error = VOP_IOCTL(vn, DIOCGDINFO, (caddr_t)dl, FREAD, NOCRED);
	if (error) {
		snprintf(errbuf, errsize,
		    "cannot read disk label, 0x%x/0x%x, error %d",
		    dev, rawdev, error);
	}
done:
	VOP_CLOSE(vn, FREAD, NOCRED);
	vput(vn);
	if (error)
		return (errbuf);
	return (NULL);
}

int
disk_map(char *path, char *mappath, int size, int flags)
{
	struct disk *dk, *mdk;
	u_char uid[8];
	char c, part;
	int i;

	/*
	 * Attempt to map a request for a disklabel UID to the correct device.
	 * We should be supplied with a disklabel UID which has the following
	 * format:
	 *
	 * [disklabel uid] . [partition]
	 *
	 * Alternatively, if the DM_OPENPART flag is set the disklabel UID can
	 * based passed on its own.
	 */

	if (strchr(path, '/') != NULL)
		return -1;

	/* Verify that the device name is properly formed. */
	if (!((strlen(path) == 16 && (flags & DM_OPENPART)) ||
	    (strlen(path) == 18 && path[16] == '.')))
		return -1;

	/* Get partition. */
	if (flags & DM_OPENPART)
		part = 'a' + RAW_PART;
	else
		part = path[17];

	if (part < 'a' || part >= 'a' + MAXPARTITIONS)
		return -1;

	/* Derive label UID. */
	memset(uid, 0, sizeof(uid));
	for (i = 0; i < 16; i++) {
		c = path[i];
		if (c >= '0' && c <= '9')
			c -= '0';
		else if (c >= 'a' && c <= 'f')
			c -= ('a' - 10);
                else
			return -1;

		uid[i / 2] <<= 4;
		uid[i / 2] |= c & 0xf;
	}

	mdk = NULL;
	TAILQ_FOREACH(dk, &disklist, dk_link) {
		if ((dk->dk_flags & DKF_LABELVALID) && dk->dk_label &&
		    memcmp(dk->dk_label->d_uid, uid,
		    sizeof(dk->dk_label->d_uid)) == 0) {
			/* Fail if there are duplicate UIDs! */
			if (mdk != NULL)
				return -1;
			mdk = dk;
		}
	}

	if (mdk == NULL || mdk->dk_name == NULL)
		return -1;

	snprintf(mappath, size, "/dev/%s%s%c",
	    (flags & DM_OPENBLCK) ? "" : "r", mdk->dk_name, part);

	return 0;
}

/*
 * Lookup a disk device and verify that it has completed attaching.
 */
struct device *
disk_lookup(struct cfdriver *cd, int unit)
{
	struct device *dv;
	struct disk *dk;

	dv = device_lookup(cd, unit);
	if (dv == NULL)
		return (NULL);

	TAILQ_FOREACH(dk, &disklist, dk_link)
		if (dk->dk_device == dv)
			break;

	if (dk == NULL) {
		device_unref(dv);
		return (NULL);
	}

	return (dv);
}
