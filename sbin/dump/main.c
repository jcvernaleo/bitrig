/*	$OpenBSD: main.c,v 1.48 2014/05/27 12:35:40 krw Exp $	*/
/*	$NetBSD: main.c,v 1.14 1997/06/05 11:13:24 lukem Exp $	*/

/*-
 * Copyright (c) 1980, 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
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
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <ufs/ffs/fs.h>
#include <ufs/ufs/dinode.h>

#include <protocols/dumprestore.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fstab.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dump.h"
#include "pathnames.h"

int	notify = 0;	/* notify operator flag */
int	blockswritten = 0;	/* number of blocks written on current tape */
int	tapeno = 0;	/* current tape number */
int	density = 0;	/* density in bytes/0.1" */
int	ntrec = NTREC;	/* # tape blocks in each tape record */
int	cartridge = 0;	/* Assume non-cartridge tape */
long	blocksperfile;	/* output blocks per file */
char	*host = NULL;	/* remote host (if any) */
int	maxbsize = 64*1024;	/* XXX MAXBSIZE from sys/param.h */

struct disklabel lab;

/*
 * Possible superblock locations ordered from most to least likely.
 */
static int sblock_try[] = SBLOCKSEARCH;

static long numarg(char *, long, long);
static void obsolete(int *, char **[]);
static void usage(void);

int
main(int argc, char *argv[])
{
	ino_t ino;
	int dirty;
	union dinode *dp;
	struct	fstab *dt;
	char *map;
	int ch, mode;
	struct tm then;
	struct statfs fsbuf;
	int i, anydirskipped, bflag = 0, Tflag = 0, honorlevel = 1;
	ino_t maxino;
	time_t t;
	int dirlist;
	char *toplevel, *str, *mount_point = NULL;

	spcl.c_date = (int64_t)time(NULL);

	tsize = 0;	/* Default later, based on 'c' option for cart tapes */
	if ((tape = getenv("TAPE")) == NULL)
		tape = _PATH_DEFTAPE;
	dumpdates = _PATH_DUMPDATES;
	temp = _PATH_DTMP;
	if (TP_BSIZE / DEV_BSIZE == 0 || TP_BSIZE % DEV_BSIZE != 0)
		quit("TP_BSIZE must be a multiple of DEV_BSIZE\n");
	level = '0';

	if (argc < 2)
		usage();

	obsolete(&argc, &argv);
	while ((ch = getopt(argc, argv, "0123456789aB:b:cd:f:h:ns:T:uWw")) != -1)
		switch (ch) {
		/* dump level */
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			level = ch;
			break;

		case 'B':		/* blocks per output file */
			blocksperfile = numarg("blocks per file", 1L, 0L);
			break;

		case 'b':		/* blocks per tape write */
			ntrec = numarg("blocks per write", 1L, 1000L);
			if (ntrec > maxbsize/1024) {
				msg("Please choose a blocksize <= %dKB\n",
				    maxbsize/1024);
				exit(X_STARTUP);
			}
			bflag = 1;
			break;

		case 'c':		/* Tape is cart. not 9-track */
			cartridge = 1;
			break;

		case 'd':		/* density, in bits per inch */
			density = numarg("density", 10L, 327670L) / 10;
			if (density >= 625 && !bflag)
				ntrec = HIGHDENSITYTREC;
			break;

		case 'f':		/* output file */
			tape = optarg;
			break;

		case 'h':
			honorlevel = numarg("honor level", 0L, 10L);
			break;

		case 'n':		/* notify operators */
			notify = 1;
			break;

		case 's':		/* tape size, feet */
			tsize = numarg("tape size", 1L, 0L) * 12 * 10;
			break;

		case 'T':		/* time of last dump */
			str = strptime(optarg, "%a %b %e %H:%M:%S %Y", &then);
			then.tm_isdst = -1;
			if (str == NULL || (*str != '\n' && *str != '\0'))
				spcl.c_ddate = -1;
			else
				spcl.c_ddate = (int64_t)mktime(&then);
			if (spcl.c_ddate < 0) {
				(void)fprintf(stderr, "bad time \"%s\"\n",
				    optarg);
				exit(X_STARTUP);
			}
			Tflag = 1;
			lastlevel = '?';
			break;

		case 'u':		/* update /etc/dumpdates */
			uflag = 1;
			break;

		case 'W':		/* what to do */
		case 'w':
			lastdump(ch);
			exit(X_FINOK);	/* do nothing else */
			break;

		case 'a':		/* `auto-size', Write to EOM. */
			unlimited = 1;
			break;

		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void)fprintf(stderr, "Must specify disk or filesystem\n");
		exit(X_STARTUP);
	}

	/*
	 *	determine if disk is a subdirectory, and setup appropriately
	 */
	dirlist = 0;
	toplevel = NULL;
	for (i = 0; i < argc; i++) {
		struct stat sb;

		if (lstat(argv[i], &sb) == -1) {
			msg("Cannot lstat %s: %s\n", argv[i], strerror(errno));
			exit(X_STARTUP);
		}
		if (!S_ISDIR(sb.st_mode) && !S_ISREG(sb.st_mode))
			break;
		if (statfs(argv[i], &fsbuf) == -1) {
			msg("Cannot statfs %s: %s\n", argv[i], strerror(errno));
			exit(X_STARTUP);
		}
		if (strcmp(argv[i], fsbuf.f_mntonname) == 0) {
			if (dirlist != 0) {
				msg("Can't dump a mountpoint and a filelist\n");
				exit(X_STARTUP);
			}
			break;		/* exit if sole mountpoint */
		}
		if (!disk) {
			if ((toplevel = strdup(fsbuf.f_mntonname)) == NULL) {
				msg("Cannot malloc diskname\n");
				exit(X_STARTUP);
			}
			disk = toplevel;
			if (uflag) {
				msg("Ignoring u flag for subdir dump\n");
				uflag = 0;
			}
			if (level > '0') {
				msg("Subdir dump is done at level 0\n");
				level = '0';
			}
			msg("Dumping sub files/directories from %s\n", disk);
		} else {
			if (strcmp(disk, fsbuf.f_mntonname) != 0) {
				msg("%s is not on %s\n", argv[i], disk);
				exit(X_STARTUP);
			}
		}
		msg("Dumping file/directory %s\n", argv[i]);
		dirlist++;
	}
	if (dirlist == 0) {
		disk = *argv++;
		if (argc != 1) {
			(void)fputs("Excess arguments to dump:", stderr);
			while (--argc) {
				(void)putc(' ', stderr);
				(void)fputs(*argv++, stderr);
			}
			(void)putc('\n', stderr);
			exit(X_STARTUP);
		}
	}
	if (Tflag && uflag) {
	        (void)fprintf(stderr,
		    "You cannot use the T and u flags together.\n");
		exit(X_STARTUP);
	}
	if (strcmp(tape, "-") == 0) {
		pipeout++;
		tape = "standard output";
	}

	if (blocksperfile)
		blocksperfile = blocksperfile / ntrec * ntrec; /* round down */
	else if (!unlimited) {
		/*
		 * Determine how to default tape size and density
		 *
		 *         	density				tape size
		 * 9-track	1600 bpi (160 bytes/.1")	2300 ft.
		 * 9-track	6250 bpi (625 bytes/.1")	2300 ft.
		 * cartridge	8000 bpi (100 bytes/.1")	1700 ft.
		 *						(450*4 - slop)
		 */
		if (density == 0)
			density = cartridge ? 100 : 160;
		if (tsize == 0)
			tsize = cartridge ? 1700L*120L : 2300L*120L;
	}

	if (strchr(tape, ':')) {
		host = tape;
		tape = strchr(host, ':');
		*tape++ = '\0';
#ifdef RDUMP
		if (rmthost(host) == 0)
			exit(X_STARTUP);
#else
		(void)fprintf(stderr, "remote dump not enabled\n");
		exit(X_STARTUP);
#endif
	}

	if (signal(SIGHUP, SIG_IGN) != SIG_IGN)
		signal(SIGHUP, sig);
	if (signal(SIGTRAP, SIG_IGN) != SIG_IGN)
		signal(SIGTRAP, sig);
	if (signal(SIGFPE, SIG_IGN) != SIG_IGN)
		signal(SIGFPE, sig);
	if (signal(SIGBUS, SIG_IGN) != SIG_IGN)
		signal(SIGBUS, sig);
	if (signal(SIGSEGV, SIG_IGN) != SIG_IGN)
		signal(SIGSEGV, sig);
	if (signal(SIGTERM, SIG_IGN) != SIG_IGN)
		signal(SIGTERM, sig);
	if (signal(SIGINT, interrupt) == SIG_IGN)
		signal(SIGINT, SIG_IGN);

	getfstab();		/* /etc/fstab snarfed */

	/*
	 *	disk can be either the full special file name,
	 *	the suffix of the special file name,
	 *	the special name missing the leading '/',
	 *	the file system name with or without the leading '/'.
	 */
	if (!statfs(disk, &fsbuf) && !strcmp(fsbuf.f_mntonname, disk)) {
		/* mounted disk? */
		disk = rawname(fsbuf.f_mntfromname);
		if (!disk) {
			(void)fprintf(stderr, "cannot get raw name for %s\n",
			    fsbuf.f_mntfromname);
			exit(X_STARTUP);
		}
		mount_point = fsbuf.f_mntonname;
		(void)strlcpy(spcl.c_dev, fsbuf.f_mntfromname,
		    sizeof(spcl.c_dev));
		if (dirlist != 0) {
			(void)snprintf(spcl.c_filesys, sizeof(spcl.c_filesys),
			    "a subset of %s", mount_point);
		} else {
			(void)strlcpy(spcl.c_filesys, mount_point,
			    sizeof(spcl.c_filesys));
		}
	} else if ((dt = fstabsearch(disk)) != NULL) {
		/* in fstab? */
		disk = rawname(dt->fs_spec);
		mount_point = dt->fs_file;
		(void)strlcpy(spcl.c_dev, dt->fs_spec, sizeof(spcl.c_dev));
		if (dirlist != 0) {
			(void)snprintf(spcl.c_filesys, sizeof(spcl.c_filesys),
			    "a subset of %s", mount_point);
		} else {
			(void)strlcpy(spcl.c_filesys, mount_point,
			    sizeof(spcl.c_filesys));
		}
	} else {
		/* must be a device */
		(void)strlcpy(spcl.c_dev, disk, sizeof(spcl.c_dev));
		(void)strlcpy(spcl.c_filesys, "an unlisted file system",
		    sizeof(spcl.c_filesys));
	}
	(void)strlcpy(spcl.c_label, "none", sizeof(spcl.c_label));
	(void)gethostname(spcl.c_host, sizeof(spcl.c_host));
	spcl.c_level = level - '0';
	spcl.c_type = TS_TAPE;
	if (!Tflag)
	        getdumptime();		/* /etc/dumpdates snarfed */

	t = (time_t)spcl.c_date;
	msg("Date of this level %c dump: %s", level,
		t == 0 ? "the epoch\n" : ctime(&t));
	t = (time_t)spcl.c_ddate;
 	msg("Date of last level %c dump: %s", lastlevel,
		t == 0 ? "the epoch\n" : ctime(&t));
	msg("Dumping %s ", disk);
	if (mount_point != NULL)
		msgtail("(%s) ", mount_point);
	if (host)
		msgtail("to %s on host %s\n", tape, host);
	else
		msgtail("to %s\n", tape);

	if ((diskfd = open(disk, O_RDONLY)) < 0) {
		msg("Cannot open %s\n", disk);
		exit(X_STARTUP);
	}
	if (ioctl(diskfd, DIOCGPDINFO, (char *)&lab) < 0)
		err(1, "ioctl (DIOCGPDINFO)");
	sync();
	sblock = (struct fs *)sblock_buf;
	for (i = 0; sblock_try[i] != -1; i++) {
		ssize_t n = pread(diskfd, sblock, SBLOCKSIZE,
		    (off_t)sblock_try[i]);
		if (n == SBLOCKSIZE && (sblock->fs_magic == FS_UFS1_MAGIC ||
		     (sblock->fs_magic == FS_UFS2_MAGIC &&
		      sblock->fs_sblockloc == sblock_try[i])) &&
		    sblock->fs_bsize <= MAXBSIZE &&
		    sblock->fs_bsize >= sizeof(struct fs))
			break;
	}
	if (sblock_try[i] == -1)
		quit("Cannot find filesystem superblock\n");
	tp_bshift = ffs(TP_BSIZE) - 1;
	if (TP_BSIZE != (1 << tp_bshift))
		quit("TP_BSIZE (%d) is not a power of 2\n", TP_BSIZE);
#ifdef FS_44INODEFMT
	if (sblock->fs_magic == FS_UFS2_MAGIC ||
	    sblock->fs_inodefmt >= FS_44INODEFMT)
		spcl.c_flags |= DR_NEWINODEFMT;
#endif
	maxino = sblock->fs_ipg * sblock->fs_ncg;
	mapsize = roundup(howmany(maxino, NBBY), TP_BSIZE);
	usedinomap = (char *)calloc((unsigned) mapsize, sizeof(char));
	dumpdirmap = (char *)calloc((unsigned) mapsize, sizeof(char));
	dumpinomap = (char *)calloc((unsigned) mapsize, sizeof(char));
	tapesize = 3 * (howmany(mapsize * sizeof(char), TP_BSIZE) + 1);

	nonodump = spcl.c_level < honorlevel;

	(void)signal(SIGINFO, statussig);

	msg("mapping (Pass I) [regular files]\n");
	anydirskipped = mapfiles(maxino, &tapesize, toplevel,
	    (dirlist ? argv : NULL));

	msg("mapping (Pass II) [directories]\n");
	while (anydirskipped) {
		anydirskipped = mapdirs(maxino, &tapesize);
	}

	if (pipeout || unlimited) {
		tapesize += 10;	/* 10 trailer blocks */
		msg("estimated %lld tape blocks.\n", tapesize);
	} else {
		double fetapes;

		if (blocksperfile)
			fetapes = (double) tapesize / blocksperfile;
		else if (cartridge) {
			/* Estimate number of tapes, assuming streaming stops at
			   the end of each block written, and not in mid-block.
			   Assume no erroneous blocks; this can be compensated
			   for with an artificially low tape size. */
			fetapes =
			(	  tapesize	/* blocks */
				* TP_BSIZE	/* bytes/block */
				* (1.0/density)	/* 0.1" / byte */
			  +
				  tapesize	/* blocks */
				* (1.0/ntrec)	/* streaming-stops per block */
				* 15.48		/* 0.1" / streaming-stop */
			) * (1.0 / tsize );	/* tape / 0.1" */
		} else {
			/* Estimate number of tapes, for old fashioned 9-track
			   tape */
			int tenthsperirg = (density == 625) ? 3 : 7;
			fetapes =
			(	  tapesize	/* blocks */
				* TP_BSIZE	/* bytes / block */
				* (1.0/density)	/* 0.1" / byte */
			  +
				  tapesize	/* blocks */
				* (1.0/ntrec)	/* IRG's / block */
				* tenthsperirg	/* 0.1" / IRG */
			) * (1.0 / tsize );	/* tape / 0.1" */
		}
		etapes = fetapes;		/* truncating assignment */
		etapes++;
		/* count the dumped inodes map on each additional tape */
		tapesize += (etapes - 1) *
			(howmany(mapsize * sizeof(char), TP_BSIZE) + 1);
		tapesize += etapes + 10;	/* headers + 10 trailer blks */
		msg("estimated %lld tape blocks on %3.2f tape(s).\n",
		    tapesize, fetapes);
	}

	/*
	 * Allocate tape buffer.
	 */
	if (!alloctape())
		quit("can't allocate tape buffers - try a smaller blocking factor.\n");

	startnewtape(1);
	(void)time(&tstart_writing);
	xferrate = 0;
	dumpmap(usedinomap, TS_CLRI, maxino - 1);

	msg("dumping (Pass III) [directories]\n");
	dirty = 0;		/* XXX just to get gcc to shut up */
	for (map = dumpdirmap, ino = 1; ino < maxino; ino++) {
		if (((ino - 1) % NBBY) == 0)	/* map is offset by 1 */
			dirty = *map++;
		else
			dirty >>= 1;
		if ((dirty & 1) == 0)
			continue;
		/*
		 * Skip directory inodes deleted and maybe reallocated
		 */
		dp = getino(ino, &mode);
		if (mode != IFDIR)
			continue;
		(void)dumpino(dp, ino);
	}

	msg("dumping (Pass IV) [regular files]\n");
	for (map = dumpinomap, ino = 1; ino < maxino; ino++) {
		if (((ino - 1) % NBBY) == 0)	/* map is offset by 1 */
			dirty = *map++;
		else
			dirty >>= 1;
		if ((dirty & 1) == 0)
			continue;
		/*
		 * Skip inodes deleted and reallocated as directories.
		 */
		dp = getino(ino, &mode);
		if (mode == IFDIR)
			continue;
		(void)dumpino(dp, ino);
	}

	spcl.c_type = TS_END;
	for (i = 0; i < ntrec; i++)
		writeheader(maxino - 1);
	if (pipeout)
		msg("%lld tape blocks\n", spcl.c_tapea);
	else
		msg("%lld tape blocks on %d volume%s\n",
		    spcl.c_tapea, spcl.c_volume,
		    (spcl.c_volume == 1) ? "" : "s");
	t = (time_t)spcl.c_date;
	msg("Date of this level %c dump: %s", level,
	    t == 0 ? "the epoch\n" : ctime(&t));
	t = do_stats();
	msg("Date this dump completed:  %s", ctime(&t));
	msg("Average transfer rate: %ld KB/s\n", xferrate / tapeno);
	putdumptime();
	trewind();
	broadcast("DUMP IS DONE!\7\7\n");
	msg("DUMP IS DONE\n");
	Exit(X_FINOK);
	/* NOTREACHED */
}

static void
usage(void)
{
	extern char *__progname;

	(void)fprintf(stderr, "usage: %s [-0123456789acnuWw] [-B records] "
		      "[-b blocksize] [-d density]\n"
		      "\t[-f file] [-h level] [-s feet] "
		      "[-T date] files-to-dump\n",
		      __progname);
	exit(X_STARTUP);
}

/*
 * Pick up a numeric argument.  It must be nonnegative and in the given
 * range (except that a vmax of 0 means unlimited).
 */
static long
numarg(char *meaning, long vmin, long vmax)
{
	long val;
	const char *errstr;

	if (vmax == 0)
		vmax = LONG_MAX;
	val = strtonum(optarg, vmin, vmax, &errstr);
	if (errstr)
		errx(X_STARTUP, "%s is %s [%ld - %ld]",
		    meaning, errstr, vmin, vmax);

	return (val);
}

void
sig(int signo)
{
	switch(signo) {
	case SIGALRM:
	case SIGBUS:
	case SIGFPE:
	case SIGHUP:
	case SIGTERM:
	case SIGTRAP:
		/* XXX signal race */
		if (pipeout)
			quit("Signal on pipe: cannot recover\n");
		msg("Rewriting attempted as response to unknown signal.\n");
		(void)fflush(stderr);
		(void)fflush(stdout);
		close_rewind();
		exit(X_REWRITE);
		/* NOTREACHED */
	case SIGSEGV:
#define SIGSEGV_MSG "SIGSEGV: ABORTING!\n"
		write(STDERR_FILENO, SIGSEGV_MSG, strlen(SIGSEGV_MSG));
		(void)signal(SIGSEGV, SIG_DFL);
		(void)kill(0, SIGSEGV);
		/* NOTREACHED */
	}
}

char *
rawname(char *cp)
{
	static char rawbuf[MAXPATHLEN];
	char *dp = strrchr(cp, '/');

	if (dp == NULL)
		return (NULL);
	*dp = '\0';
	(void)snprintf(rawbuf, sizeof(rawbuf), "%s/r%s", cp, dp + 1);
	*dp = '/';
	return (rawbuf);
}

/*
 * obsolete --
 *	Change set of key letters and ordered arguments into something
 *	getopt(3) will like.
 */
static void
obsolete(int *argcp, char **argvp[])
{
	int argc, flags;
	char *ap, **argv, *flagsp, **nargv, *p;
	size_t len;

	/* Setup. */
	argv = *argvp;
	argc = *argcp;

	/* Return if no arguments or first argument has leading dash. */
	ap = argv[1];
	if (argc == 1 || *ap == '-')
		return;

	/* Allocate space for new arguments. */
	if ((*argvp = nargv = calloc(argc + 1, sizeof(char *))) == NULL ||
	    (p = flagsp = malloc(strlen(ap) + 2)) == NULL)
		err(1, NULL);

	*nargv++ = *argv;
	argv += 2;

	for (flags = 0; *ap; ++ap) {
		switch (*ap) {
		case 'B':
		case 'b':
		case 'd':
		case 'f':
		case 'h':
		case 's':
		case 'T':
			if (*argv == NULL) {
				warnx("option requires an argument -- %c", *ap);
				usage();
			}
			len = 2 + strlen(*argv) + 1;
			if ((nargv[0] = malloc(len)) == NULL)
				err(1, NULL);
			nargv[0][0] = '-';
			nargv[0][1] = *ap;
			(void)strlcpy(&nargv[0][2], *argv, len - 2);
			++argv;
			++nargv;
			break;
		default:
			if (!flags) {
				*p++ = '-';
				flags = 1;
			}
			*p++ = *ap;
			break;
		}
	}

	/* Terminate flags, or toss the buffer we did not use. */
	if (flags) {
		*p = '\0';
		*nargv++ = flagsp;
	} else
		free(flagsp);

	/* Copy remaining arguments. */
	while ((*nargv++ = *argv++))
		;

	/* Update argument count. */
	*argcp = nargv - *argvp - 1;
}
