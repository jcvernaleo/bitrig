/* $OpenBSD: omdog.c,v 1.5 2011/11/15 23:01:11 drahn Exp $ */
/*
 * Copyright (c) 2012-2013 Patrick Wildt <patrick@blueri.se>
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/evcount.h>
#include <sys/socket.h>
#include <sys/timeout.h>
#include <machine/intr.h>
#include <machine/bus.h>
#include <machine/fdt.h>
#include <armv7/armv7/armv7var.h>

/* registers */
#define WCR		0x00
#define WSR		0x02
#define WRSR		0x04
#define WICR		0x06
#define WMCR		0x08

struct imxdog_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
};

struct imxdog_softc *imxdog_sc;

int imxdog_match(struct device *parent, void *v, void *aux);
void imxdog_attach(struct device *parent, struct device *self, void *args);
void imxdog_reset(void);

struct cfattach	imxdog_ca = {
	sizeof (struct imxdog_softc), NULL, imxdog_attach
};
struct cfattach	imxdog_fdt_ca = {
	sizeof (struct imxdog_softc), imxdog_match, imxdog_attach
};

struct cfdriver imxdog_cd = {
	NULL, "imxdog", DV_DULL
};

int
imxdog_match(struct device *parent, void *v, void *aux)
{
	struct armv7_attach_args *aa = aux;

	if (fdt_node_compatible("fsl,imx6q-wdt", aa->aa_node))
		return 1;

	return 0;
}

void
imxdog_attach(struct device *parent, struct device *self, void *args)
{
	struct armv7_attach_args *aa = args;
	struct imxdog_softc *sc = (struct imxdog_softc *) self;
	struct fdt_memory mem;

	sc->sc_iot = aa->aa_iot;
	if (aa->aa_node) {
		if (fdt_get_memory_address(aa->aa_node, 0, &mem))
			panic("%s: could not extract memory data from FDT",
			    __func__);
	} else {
		mem.addr = aa->aa_dev->mem[0].addr;
		mem.size = aa->aa_dev->mem[0].size;
	}

	if (bus_space_map(sc->sc_iot, mem.addr, mem.size, 0, &sc->sc_ioh))
		panic("%s: bus_space_map failed!", __func__);

	printf("\n");
	imxdog_sc = sc;
}

void
imxdog_reset()
{
	if (imxdog_sc == NULL)
		return;

	/* disable watchdog and set timeout to 0 */
	bus_space_write_2(imxdog_sc->sc_iot, imxdog_sc->sc_ioh, WCR, 0);

	/* sequence to reset timeout counter */
	bus_space_write_2(imxdog_sc->sc_iot, imxdog_sc->sc_ioh, WSR, 0x5555);
	bus_space_write_2(imxdog_sc->sc_iot, imxdog_sc->sc_ioh, WSR, 0xaaaa);

	/* enable watchdog */
	bus_space_write_2(imxdog_sc->sc_iot, imxdog_sc->sc_ioh, WCR, 1);
	/* errata TKT039676 */
	bus_space_write_2(imxdog_sc->sc_iot, imxdog_sc->sc_ioh, WCR, 1);

	delay(100000);
}
