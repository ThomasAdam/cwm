/* $OpenBSD$ */

/*
 * Copyright (c) 2011 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>

#include "calmwm.h"

/* This file contains screen/group/client options as defaults.  These are
 * copied in to global structures which are the overriden by user-defined ones
 * later on.
 */

const struct options_table_entry options_table[] = {
	{ .name = "border-width",
	  .type = OPTIONS_TABLE_NUMBER,
	  .scope = OPTIONS_TABLE_GROUP | OPTIONS_TABLE_CLIENT,
	  .default_num = 4,
	  .minimum = 0,
	  .maximum = INT_MAX,
	},
	{ .name = "gap",
	  .type = OPTIONS_TABLE_STRING,
	  .scope = OPTIONS_TABLE_SCREEN,
	  .default_str = "0,0,0,0",
	},
	{ .name = "move-amount",
	  .type = OPTIONS_TABLE_NUMBER,
	  .default_num = 1,
	  .minimum = 1,
	  .maximum = INT_MAX,
	},
	{ .name = NULL }
};
