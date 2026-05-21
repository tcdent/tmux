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

#include <stdlib.h>

#include "tmux.h"

/*
 * Swap two panes.
 */

static enum cmd_retval	cmd_swap_pane_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_swap_pane_entry = {
	.name = "swap-pane",
	.alias = "swapp",

	.args = { "dDs:t:UZ", 0, 0, NULL },
	.usage = "[-dDUZ] " CMD_SRCDST_PANE_USAGE,

	.source = { 's', CMD_FIND_PANE, CMD_FIND_DEFAULT_MARKED },
	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_swap_pane_exec
};

static enum cmd_retval
cmd_swap_pane_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*source = cmdq_get_source(item);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window		*src_w, *dst_w;
	struct window_pane	*tmp_wp, *src_wp, *dst_wp;
	struct panelink		*src_pl, *dst_pl, *tmp_pl;
	struct layout_cell	*src_lc, *dst_lc;
	u_int			 sx, sy, xoff, yoff;

	dst_w = target->wl->window;
	dst_wp = target->wp;
	src_w = source->wl->window;
	src_wp = source->wp;

	if (window_push_zoom(dst_w, 0, args_has(args, 'Z')))
		server_redraw_window(dst_w);

	if (args_has(args, 'D')) {
		src_w = dst_w;
		tmp_pl = panelink_find_by_pane(&dst_w->panes, dst_wp);
		tmp_pl = (tmp_pl != NULL) ? TAILQ_NEXT(tmp_pl, entry) : NULL;
		if (tmp_pl == NULL)
			tmp_pl = TAILQ_FIRST(&dst_w->panes);
		src_wp = (tmp_pl != NULL) ? tmp_pl->pane : NULL;
	} else if (args_has(args, 'U')) {
		src_w = dst_w;
		tmp_pl = panelink_find_by_pane(&dst_w->panes, dst_wp);
		tmp_pl = (tmp_pl != NULL) ?
		    TAILQ_PREV(tmp_pl, panelinks, entry) : NULL;
		if (tmp_pl == NULL)
			tmp_pl = TAILQ_LAST(&dst_w->panes, panelinks);
		src_wp = (tmp_pl != NULL) ? tmp_pl->pane : NULL;
	}

	if (src_w != dst_w && window_push_zoom(src_w, 0, args_has(args, 'Z')))
		server_redraw_window(src_w);

	if (src_wp == dst_wp)
		goto out;

	if ((src_wp->flags & PANE_FLOATING) &&
	    (dst_wp->flags & PANE_FLOATING)) {
		cmdq_error(item, "cannot swap floating panes");
		return (CMD_RETURN_ERROR);
	}

	server_client_remove_pane(src_wp);
	server_client_remove_pane(dst_wp);

	src_pl = panelink_find_by_pane(&src_w->panes, src_wp);
	dst_pl = panelink_find_by_pane(&dst_w->panes, dst_wp);

	tmp_pl = TAILQ_PREV(dst_pl, panelinks, entry);
	TAILQ_REMOVE(&dst_w->panes, dst_pl, entry);
	TAILQ_REPLACE(&src_w->panes, src_pl, dst_pl, entry);
	if (tmp_pl == src_pl)
		tmp_pl = dst_pl;
	if (tmp_pl == NULL)
		TAILQ_INSERT_HEAD(&dst_w->panes, src_pl, entry);
	else
		TAILQ_INSERT_AFTER(&dst_w->panes, tmp_pl, src_pl, entry);

	src_lc = src_pl->layout_cell;
	dst_lc = dst_pl->layout_cell;
	src_lc->pl = dst_pl;
	dst_pl->layout_cell = src_lc;
	dst_lc->pl = src_pl;
	src_pl->layout_cell = dst_lc;
	if ((src_wp->flags ^ dst_wp->flags) & PANE_FLOATING) {
		src_wp->flags ^= PANE_FLOATING;
		dst_wp->flags ^= PANE_FLOATING;
	}

	src_pl->window = dst_w;
	dst_pl->window = src_w;

	src_wp->window = dst_w;
	options_set_parent(src_wp->options, dst_w->options);
	src_wp->flags |= (PANE_STYLECHANGED|PANE_THEMECHANGED);
	dst_wp->window = src_w;
	options_set_parent(dst_wp->options, src_w->options);
	dst_wp->flags |= (PANE_STYLECHANGED|PANE_THEMECHANGED);

	sx = src_wp->sx; sy = src_wp->sy;
	xoff = src_wp->xoff; yoff = src_wp->yoff;
	src_wp->xoff = dst_wp->xoff; src_wp->yoff = dst_wp->yoff;
	window_pane_resize(src_wp, dst_wp->sx, dst_wp->sy);
	dst_wp->xoff = xoff; dst_wp->yoff = yoff;
	window_pane_resize(dst_wp, sx, sy);

	if (!args_has(args, 'd')) {
		if (src_w != dst_w) {
			window_set_active_pane(src_w, dst_wp, 1);
			window_set_active_pane(dst_w, src_wp, 1);
		} else {
			tmp_wp = dst_wp;
			window_set_active_pane(src_w, tmp_wp, 1);
		}
	} else {
		if (src_w->active == src_wp)
			window_set_active_pane(src_w, dst_wp, 1);
		if (dst_w->active == dst_wp)
			window_set_active_pane(dst_w, src_wp, 1);
	}
	if (src_w != dst_w) {
		window_pane_stack_remove(src_w, src_wp);
		window_pane_stack_remove(dst_w, dst_wp);
		colour_palette_from_option(&src_wp->palette, src_wp->options);
		colour_palette_from_option(&dst_wp->palette, dst_wp->options);
		layout_fix_panes(src_w, NULL);
		server_redraw_window(src_w);
	}
	layout_fix_panes(dst_w, NULL);
	server_redraw_window(dst_w);

	notify_window("window-layout-changed", src_w);
	if (src_w != dst_w)
		notify_window("window-layout-changed", dst_w);

out:
	if (window_pop_zoom(src_w))
		server_redraw_window(src_w);
	if (src_w != dst_w && window_pop_zoom(dst_w))
		server_redraw_window(dst_w);
	return (CMD_RETURN_NORMAL);
}
