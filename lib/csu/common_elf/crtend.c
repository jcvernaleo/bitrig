/*	$OpenBSD: crtend.c,v 1.3 2004/01/08 14:59:15 drahn Exp $	*/
/*	$NetBSD: crtend.c,v 1.1 1996/09/12 16:59:04 cgd Exp $	*/

#include <sys/cdefs.h>
#include "md_init.h"

static void (*__CTOR_LIST__[1])(void)
    __attribute__((section(".ctors"))) = { (void *)0 };		/* XXX */
static void (*__DTOR_LIST__[1])(void)
    __attribute__((section(".dtors"))) = { (void *)0 };		/* XXX */

MD_SECTION_EPILOGUE(".init");
MD_SECTION_EPILOGUE(".fini");
