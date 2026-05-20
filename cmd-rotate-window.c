/* $OpenBSD$ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include "tmux.h"

/*
 * Rotate the panes in a window.
 */

static enum cmd_retval	cmd_rotate_window_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_rotate_window_entry = {
	.name = "rotate-window",
	.alias = "rotatew",

	.args = { "Dt:UZ", 0, 0, NULL },
	.usage = "[-DUZ] " CMD_TARGET_WINDOW_USAGE,

	.target = { 't', CMD_FIND_WINDOW, 0 },

	.flags = 0,
	.exec = cmd_rotate_window_exec
};

static enum cmd_retval
cmd_rotate_window_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*current = cmdq_get_current(item);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct winlink		*wl = target->wl;
	struct window		*w = wl->window;
	struct window_pane	*wp, *wp2;
	struct panelink		*pl, *pl2;
	struct layout_cell	*lc;
	u_int			 sx, sy, xoff, yoff;

	window_push_zoom(w, 0, args_has(args, 'Z'));

	if (args_has(args, 'D')) {
		pl = TAILQ_LAST(&w->panes, panelinks);
		TAILQ_REMOVE(&w->panes, pl, entry);
		TAILQ_INSERT_HEAD(&w->panes, pl, entry);

		wp = pl->pane;
		lc = wp->layout_cell;
		xoff = wp->xoff; yoff = wp->yoff;
		sx = wp->sx; sy = wp->sy;
		TAILQ_FOREACH(pl, &w->panes, entry) {
			wp = pl->pane;
			if ((pl2 = TAILQ_NEXT(pl, entry)) == NULL)
				break;
			wp2 = pl2->pane;
			wp->layout_cell = wp2->layout_cell;
			if (wp->layout_cell != NULL)
				wp->layout_cell->wp = wp;
			wp->xoff = wp2->xoff; wp->yoff = wp2->yoff;
			window_pane_resize(wp, wp2->sx, wp2->sy);
		}
		wp->layout_cell = lc;
		if (wp->layout_cell != NULL)
			wp->layout_cell->wp = wp;
		wp->xoff = xoff; wp->yoff = yoff;
		window_pane_resize(wp, sx, sy);

		pl = panelink_find_by_pane(&w->panes, w->active);
		pl = (pl != NULL) ? TAILQ_PREV(pl, panelinks, entry) : NULL;
		if (pl == NULL)
			pl = TAILQ_LAST(&w->panes, panelinks);
		wp = (pl != NULL) ? pl->pane : NULL;
	} else {
		pl = TAILQ_FIRST(&w->panes);
		TAILQ_REMOVE(&w->panes, pl, entry);
		TAILQ_INSERT_TAIL(&w->panes, pl, entry);

		wp = pl->pane;
		lc = wp->layout_cell;
		xoff = wp->xoff; yoff = wp->yoff;
		sx = wp->sx; sy = wp->sy;
		TAILQ_FOREACH_REVERSE(pl, &w->panes, panelinks, entry) {
			wp = pl->pane;
			if ((pl2 = TAILQ_PREV(pl, panelinks, entry)) == NULL)
				break;
			wp2 = pl2->pane;
			wp->layout_cell = wp2->layout_cell;
			if (wp->layout_cell != NULL)
				wp->layout_cell->wp = wp;
			wp->xoff = wp2->xoff; wp->yoff = wp2->yoff;
			window_pane_resize(wp, wp2->sx, wp2->sy);
		}
		wp->layout_cell = lc;
		if (wp->layout_cell != NULL)
			wp->layout_cell->wp = wp;
		wp->xoff = xoff; wp->yoff = yoff;
		window_pane_resize(wp, sx, sy);

		pl = panelink_find_by_pane(&w->panes, w->active);
		pl = (pl != NULL) ? TAILQ_NEXT(pl, entry) : NULL;
		if (pl == NULL)
			pl = TAILQ_FIRST(&w->panes);
		wp = (pl != NULL) ? pl->pane : NULL;
	}

	window_set_active_pane(w, wp, 1);
	cmd_find_from_winlink_pane(current, wl, wp, 0);
	window_pop_zoom(w);
	server_redraw_window(w);

	return (CMD_RETURN_NORMAL);
}
