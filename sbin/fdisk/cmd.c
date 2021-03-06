/*	$OpenBSD: cmd.c,v 1.82 2015/04/02 18:00:55 krw Exp $	*/

/*
 * Copyright (c) 1997 Tobias Weingartner
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/disklabel.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#include "disk.h"
#include "misc.h"
#include "part.h"
#include "mbr.h"
#include "user.h"
#include "cmd.h"

int reinited;

int
Xreinit(char *args, struct mbr *mbr)
{
	struct dos_mbr dos_mbr;

	MBR_make(&initial_mbr, &dos_mbr);
	MBR_parse(&dos_mbr, mbr->offset, mbr->reloffset, mbr);

	MBR_init(mbr);
	reinited = 1;

	/* Tell em we did something */
	printf("In memory copy is initialized to:\n");
	MBR_print(mbr, args);
	printf("Use 'write' to update disk.\n");

	return (CMD_DIRTY);
}

int
Xdisk(char *args, struct mbr *mbr)
{
	int maxcyl  = 1024;
	int maxhead = 256;
	int maxsec  = 63;

	/* Print out disk info */
	DISK_printgeometry(args);

#if defined (__powerpc__) || defined (__mips__)
	maxcyl  = 9999999;
	maxhead = 9999999;
	maxsec  = 9999999;
#endif

	/* Ask for new info */
	if (ask_yn("Change disk geometry?")) {
		disk.cylinders = ask_num("BIOS Cylinders",
		    disk.cylinders, 1, maxcyl);
		disk.heads = ask_num("BIOS Heads",
		    disk.heads, 1, maxhead);
		disk.sectors = ask_num("BIOS Sectors",
		    disk.sectors, 1, maxsec);

		disk.size = disk.cylinders * disk.heads * disk.sectors;
	}

	return (CMD_CONT);
}

int
Xswap(char *args, struct mbr *mbr)
{
	const char *errstr;
	char *from, *to;
	int pf, pt;
	struct prt pp;

	to = args;
	from = strsep(&to, " \t");

	if (to == NULL) {
		printf("partition number is invalid:\n");
		return (CMD_CONT);
	}

	pf = strtonum(from, 0, 3, &errstr);
	if (errstr) {
		printf("partition number is %s: %s\n", errstr, from);
		return (CMD_CONT);
	}
	pt = strtonum(to, 0, 3, &errstr);
	if (errstr) {
		printf("partition number is %s: %s\n", errstr, to);
		return (CMD_CONT);
	}

	if (pt == pf) {
		printf("%d same partition as %d, doing nothing.\n", pt, pf);
		return (CMD_CONT);
	}

	pp = mbr->part[pt];
	mbr->part[pt] = mbr->part[pf];
	mbr->part[pf] = pp;

	return (CMD_DIRTY);
}

int
Xedit(char *args, struct mbr *mbr)
{
	const char *errstr;
	int pn, ret, new;
	uint32_t num;
	struct prt *pp;

	pn = strtonum(args, 0, 3, &errstr);
	if (errstr) {
		printf("partition number is %s: %s\n", errstr, args);
		return (CMD_CONT);
	}
	pp = &mbr->part[pn];

	new = (pp->id == DOSPTYP_UNUSED);

	/* Edit partition type */
	ret = Xsetpid(args, mbr);

	/* Unused, so just zero out */
	if (pp->id == DOSPTYP_UNUSED) {
		memset(pp, 0, sizeof(*pp));
		printf("Partition %d is disabled.\n", pn);
		return (ret);
	}

	/* Use remaining free space for new partitions */
	if (new)
		MBR_fillremaining(mbr, pn);

	/* Change table entry */
	if (ask_yn("Do you wish to edit in CHS mode?")) {
		int maxcyl, maxhead, maxsect;

		/* Shorter */
		maxcyl = disk.cylinders - 1;
		maxhead = disk.heads - 1;
		maxsect = disk.sectors;

		/* Get data */
#define	EDIT(p, v, n, m)			\
	if ((num = ask_num(p, v, n, m)) != v)	\
		ret = CMD_DIRTY;		\
	v = num;
		EDIT("BIOS Starting cylinder", pp->scyl,  0, maxcyl);
		EDIT("BIOS Starting head",     pp->shead, 0, maxhead);
		EDIT("BIOS Starting sector",   pp->ssect, 1, maxsect);

		if (ret == CMD_DIRTY) {
			pp->bs = CHS_to_BN(pp->scyl, pp->shead, pp->ssect);
			MBR_grow_part(mbr, pn);
		}

		EDIT("BIOS Ending cylinder",   pp->ecyl,  0, maxcyl);
		EDIT("BIOS Ending head",       pp->ehead, 0, maxhead);
		EDIT("BIOS Ending sector",     pp->esect, 1, maxsect);
#undef EDIT
		/* Fix up off/size values */
		PRT_fix_BN(pp, pn);
		/* Fix up CHS values for LBA */
		PRT_fix_CHS(pp);
	} else {
#define	EDIT_BN(p, v, n)				\
	if ((num = getuint64(p, v, n)) != v)	\
		ret = CMD_DIRTY;			\
	v = num;
		EDIT_BN("Partition offset", pp->bs, (uint64_t)disk.size);

		if (ret == CMD_DIRTY)
			MBR_grow_part(mbr, pn);

		EDIT_BN("Partition size", pp->ns,
		    (uint64_t)(disk.size - pp->bs));
#undef EDIT_BN

		/* Fix up CHS values */
		PRT_fix_CHS(pp);
	}

	return (ret);
}

int
Xsetpid(char *args, struct mbr *mbr)
{
	const char *errstr;
	int pn, num;
	struct prt *pp;

	pn = strtonum(args, 0, 3, &errstr);
	if (errstr) {
		printf("partition number is %s: %s\n", errstr, args);
		return (CMD_CONT);
	}
	pp = &mbr->part[pn];

	/* Print out current table entry */
	PRT_print(0, NULL, NULL);
	PRT_print(pn, pp, NULL);

	/* Ask for MBR partition type */
	num = ask_pid((pp->id == DOSPTYP_UNUSED) ? DOSPTYP_OPENBSD : pp->id,
	    0x01, 0xff);
	if (num == pp->id)
		return (CMD_CONT);

	pp->id = num;

	return (CMD_DIRTY);
}

int
Xselect(char *args, struct mbr *mbr)
{
	const char *errstr;
	static off_t firstoff = 0;
	off_t off;
	int pn;

	pn = strtonum(args, 0, 3, &errstr);
	if (errstr) {
		printf("partition number is %s: %s\n", errstr, args);
		return (CMD_CONT);
	}

	off = mbr->part[pn].bs;

	/* Sanity checks */
	if ((mbr->part[pn].id != DOSPTYP_EXTEND) &&
	    (mbr->part[pn].id != DOSPTYP_EXTENDL)) {
		printf("Partition %d is not an extended partition.\n", pn);
		return (CMD_CONT);
	}

	if (firstoff == 0)
		firstoff = off;

	if (!off) {
		printf("Loop to offset 0!  Not selected.\n");
		return (CMD_CONT);
	} else {
		printf("Selected extended partition %d\n", pn);
		printf("New MBR at offset %lld.\n", (long long)off);
	}

	/* Recursion is beautiful! */
	USER_edit(off, firstoff);

	return (CMD_CONT);
}

int
Xprint(char *args, struct mbr *mbr)
{

	DISK_printgeometry(args);
	MBR_print(mbr, args);

	return (CMD_CONT);
}

int
Xwrite(char *args, struct mbr *mbr)
{
	struct dos_mbr dos_mbr;
	int fd;

	if (MBR_verify(mbr))
		return (CMD_CONT);

	fd = DISK_open(disk.name, O_RDWR);
	MBR_make(mbr, &dos_mbr);

	printf("Writing MBR at offset %lld.\n", (long long)mbr->offset);
	if (MBR_write(fd, mbr->offset, &dos_mbr) == -1) {
		int saved_errno = errno;
		warn("error writing MBR");
		close(fd);
		errno = saved_errno;
		return (CMD_CONT);
	}

	/* Make sure GPT doesn't get in the way. */
	if (reinited)
		MBR_zapgpt(fd, &dos_mbr, DL_GETDSIZE(&dl) - 1);

	/* Refresh in memory copy to reflect what was just written. */
	MBR_parse(&dos_mbr, mbr->offset, mbr->reloffset, mbr);

	close(fd);

	return (CMD_CLEAN);
}

int
Xquit(char *args, struct mbr *mbr)
{
	return (CMD_SAVE);
}

int
Xabort(char *args, struct mbr *mbr)
{
	exit(0);
}

int
Xexit(char *args, struct mbr *mbr)
{
	return (CMD_EXIT);
}

int
Xhelp(char *args, struct mbr *mbr)
{
	int i;

	for (i = 0; cmd_table[i].cmd != NULL; i++)
		printf("\t%s\t\t%s\n", cmd_table[i].cmd, cmd_table[i].help);
	return (CMD_CONT);
}

int
Xupdate(char *args, struct mbr *mbr)
{
	/* Update code */
	memcpy(mbr->code, initial_mbr.code, sizeof(mbr->code));
	mbr->signature = DOSMBR_SIGNATURE;
	printf("Machine code updated.\n");
	return (CMD_DIRTY);
}

int
Xflag(char *args, struct mbr *mbr)
{
	const char *errstr;
	int i, pn = -1, val = -1;
	char *part, *flag;

	flag = args;
	part = strsep(&flag, " \t");

	pn = strtonum(part, 0, 3, &errstr);
	if (errstr) {
		printf("partition number is %s: %s.\n", errstr, part);
		return (CMD_CONT);
	}

	if (flag != NULL) {
		val = (int)strtonum(flag, 0, 0xff, &errstr);
		if (errstr) {
			printf("flag value is %s: %s.\n", errstr, flag);
			return (CMD_CONT);
		}
	}

	if (val == -1) {
		/* Set active flag */
		for (i = 0; i < 4; i++) {
			if (i == pn)
				mbr->part[i].flag = DOSACTIVE;
			else
				mbr->part[i].flag = 0x00;
		}
		printf("Partition %d marked active.\n", pn);
	} else {
		mbr->part[pn].flag = val;
		printf("Partition %d flag value set to 0x%x.\n", pn, val);
	}

	return (CMD_DIRTY);
}

int
Xmanual(char *args, struct mbr *mbr)
{
	char *pager = "/usr/bin/less";
	char *p;
	sig_t opipe;
	extern const unsigned char manpage[];
	extern const int manpage_sz;
	FILE *f;

	opipe = signal(SIGPIPE, SIG_IGN);
	if ((p = getenv("PAGER")) != NULL && (*p != '\0'))
		pager = p;
	if (asprintf(&p, "gunzip -qc|%s", pager) != -1) {
		f = popen(p, "w");
		if (f) {
			fwrite(manpage, manpage_sz, 1, f);
			pclose(f);
		}
		free(p);
	}

	signal(SIGPIPE, opipe);

	return (CMD_CONT);
}
