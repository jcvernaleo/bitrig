/*	$OpenBSD: cl_main.c,v 1.26 2015/03/29 01:04:23 bcallah Exp $	*/

/*-
 * Copyright (c) 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1993, 1994, 1995, 1996
 *	Keith Bostic.  All rights reserved.
 *
 * See the LICENSE file for redistribution information.
 */

#include <sys/types.h>
#include <sys/queue.h>

#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>
#include <termios.h>
#include <unistd.h>

#include "../common/common.h"
#include "cl.h"

GS *__global_list;				/* GLOBAL: List of screens. */
sigset_t __sigblockset;				/* GLOBAL: Blocked signals. */

static CL_PRIVATE *cl_init(GS *);
static GS	  *gs_init(char *);
static void	   perr(char *, char *);
static int	   setsig(int, struct sigaction *, void (*)(int));
static void	   sig_end(GS *);
static void	   term_init(char *, char *);

/*
 * main --
 *	This is the main loop for the standalone curses editor.
 */
int
main(int argc, char *argv[])
{
	static int reenter;
	CL_PRIVATE *clp;
	GS *gp;
	size_t rows, cols;
	int rval;
	char *ttype;

	/* If loaded at 0 and jumping through a NULL pointer, stop. */
	if (reenter++)
		abort();

	/* Create and initialize the global structure. */
	__global_list = gp = gs_init(argv[0]);

	/* Create and initialize the CL_PRIVATE structure. */
	clp = cl_init(gp);

	/*
	 * Initialize the terminal information.
	 *
	 * We have to know what terminal it is from the start, since we may
	 * have to use termcap/terminfo to find out how big the screen is.
	 */
	if ((ttype = getenv("TERM")) == NULL)
		ttype = "unknown";
	term_init(gp->progname, ttype);

	/* Add the terminal type to the global structure. */
	if ((OG_D_STR(gp, GO_TERM) =
	    OG_STR(gp, GO_TERM) = strdup(ttype)) == NULL)
		perr(gp->progname, NULL);

	/* Figure out how big the screen is. */
	if (cl_ssize(NULL, 0, &rows, &cols, NULL))
		exit (1);

	/* Add the rows and columns to the global structure. */
	OG_VAL(gp, GO_LINES) = OG_D_VAL(gp, GO_LINES) = rows;
	OG_VAL(gp, GO_COLUMNS) = OG_D_VAL(gp, GO_COLUMNS) = cols;

	/* Ex wants stdout to be buffered. */
	(void)setvbuf(stdout, NULL, _IOFBF, 0);

	/* Start catching signals. */
	if (sig_init(gp, NULL))
		exit (1);

	/* Run ex/vi. */
	rval = editor(gp, argc, argv);

	/* Clean up signals. */
	sig_end(gp);

	/* Clean up the terminal. */
	(void)cl_quit(gp);

	/*
	 * XXX
	 * Reset the O_MESG option.
	 */
	if (clp->tgw != TGW_UNKNOWN)
		(void)cl_omesg(NULL, clp, clp->tgw == TGW_SET);

	/*
	 * XXX
	 * Reset the X11 xterm icon/window name.
	 */
	if (F_ISSET(clp, CL_RENAME)) {
		(void)printf(XTERM_RENAME, ttype);
		(void)fflush(stdout);
	}

	/* If a killer signal arrived, pretend we just got it. */
	if (clp->killersig) {
		(void)signal(clp->killersig, SIG_DFL);
		(void)kill(getpid(), clp->killersig);
		/* NOTREACHED */
	}

	/* Free the global and CL private areas. */
#if defined(DEBUG) || defined(PURIFY)
	free(clp);
	free(gp);
#endif

	exit (rval);
}

/*
 * gs_init --
 *	Create and partially initialize the GS structure.
 */
static GS *
gs_init(char *name)
{
	GS *gp;
	char *p;

	/* Figure out what our name is. */
	if ((p = strrchr(name, '/')) != NULL)
		name = p + 1;

	/* Allocate the global structure. */
	gp = calloc(1, sizeof(GS));
	if (gp == NULL)
		perr(name, NULL);

	gp->progname = name;
	return (gp);
}

/*
 * cl_init --
 *	Create and partially initialize the CL structure.
 */
static CL_PRIVATE *
cl_init(GS *gp)
{
	CL_PRIVATE *clp;
	int fd;

	/* Allocate the CL private structure. */
	clp = calloc(1, sizeof(CL_PRIVATE));
	if (clp == NULL)
		perr(gp->progname, NULL);
	gp->cl_private = clp;

	/*
	 * Set the CL_STDIN_TTY flag.  It's purpose is to avoid setting
	 * and resetting the tty if the input isn't from there.  We also
	 * use the same test to determine if we're running a script or
	 * not.
	 */
	if (isatty(STDIN_FILENO))
		F_SET(clp, CL_STDIN_TTY);
	else
		F_SET(gp, G_SCRIPTED);

	/*
	 * We expect that if we've lost our controlling terminal that the
	 * open() (but not the tcgetattr()) will fail.
	 */
	if (F_ISSET(clp, CL_STDIN_TTY)) {
		if (tcgetattr(STDIN_FILENO, &clp->orig) == -1)
			goto tcfail;
	} else if ((fd = open(_PATH_TTY, O_RDONLY, 0)) != -1) {
		if (tcgetattr(fd, &clp->orig) == -1) {
tcfail:			perr(gp->progname, "tcgetattr");
			exit (1);
		}
		(void)close(fd);
	}

	return (clp);
}

/*
 * term_init --
 *	Initialize terminal information.
 */
static void
term_init(char *name, char *ttype)
{
	int err;

	/* Set up the terminal database information. */
	setupterm(ttype, STDOUT_FILENO, &err);
	switch (err) {
	case -1:
		(void)fprintf(stderr,
		    "%s: No terminal database found\n", name);
		exit (1);
	case 0:
		(void)fprintf(stderr,
		    "%s: %s: unknown terminal type\n", name, ttype);
		exit (1);
	}
}

#define	GLOBAL_CLP \
	CL_PRIVATE *clp = GCLP(__global_list);
static void
h_hup(int signo)
{
	GLOBAL_CLP;

	F_SET(clp, CL_SIGHUP);
	clp->killersig = SIGHUP;
}

static void
h_int(int signo)
{
	GLOBAL_CLP;

	F_SET(clp, CL_SIGINT);
}

static void
h_term(int signo)
{
	GLOBAL_CLP;

	F_SET(clp, CL_SIGTERM);
	clp->killersig = SIGTERM;
}

static void
h_winch(int signo)
{
	GLOBAL_CLP;

	F_SET(clp, CL_SIGWINCH);
}
#undef	GLOBAL_CLP

/*
 * sig_init --
 *	Initialize signals.
 */
int
sig_init(GS *gp, SCR *sp)
{
	CL_PRIVATE *clp;

	clp = GCLP(gp);

	if (sp == NULL) {
		(void)sigemptyset(&__sigblockset);
		if (sigaddset(&__sigblockset, SIGHUP) ||
		    setsig(SIGHUP, &clp->oact[INDX_HUP], h_hup) ||
		    sigaddset(&__sigblockset, SIGINT) ||
		    setsig(SIGINT, &clp->oact[INDX_INT], h_int) ||
		    sigaddset(&__sigblockset, SIGTERM) ||
		    setsig(SIGTERM, &clp->oact[INDX_TERM], h_term) ||
		    sigaddset(&__sigblockset, SIGWINCH) ||
		    setsig(SIGWINCH, &clp->oact[INDX_WINCH], h_winch)
		    ) {
			perr(gp->progname, NULL);
			return (1);
		}
	} else
		if (setsig(SIGHUP, NULL, h_hup) ||
		    setsig(SIGINT, NULL, h_int) ||
		    setsig(SIGTERM, NULL, h_term) ||
		    setsig(SIGWINCH, NULL, h_winch)
		    ) {
			msgq(sp, M_SYSERR, "signal-reset");
		}
	return (0);
}

/*
 * setsig --
 *	Set a signal handler.
 */
static int
setsig(int signo, struct sigaction *oactp, void (*handler)(int))
{
	struct sigaction act;

	/*
	 * Use sigaction(2), not signal(3), since we don't always want to
	 * restart system calls.  The example is when waiting for a command
	 * mode keystroke and SIGWINCH arrives.  Besides, you can't portably
	 * restart system calls (thanks, POSIX!).
	 */
	act.sa_handler = handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	return (sigaction(signo, &act, oactp));
}

/*
 * sig_end --
 *	End signal setup.
 */
static void
sig_end(GS *gp)
{
	CL_PRIVATE *clp;

	clp = GCLP(gp);
	(void)sigaction(SIGHUP, NULL, &clp->oact[INDX_HUP]);
	(void)sigaction(SIGINT, NULL, &clp->oact[INDX_INT]);
	(void)sigaction(SIGTERM, NULL, &clp->oact[INDX_TERM]);
	(void)sigaction(SIGWINCH, NULL, &clp->oact[INDX_WINCH]);
}

/*
 * perr --
 *	Print system error.
 */
static void
perr(char *name, char *msg)
{
	(void)fprintf(stderr, "%s:", name);
	if (msg != NULL)
		(void)fprintf(stderr, "%s:", msg);
	(void)fprintf(stderr, "%s\n", strerror(errno));
	exit(1);
}
