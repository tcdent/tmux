/* $OpenBSD$ */

/*
 * Copyright (c) 2024 shared-panes prototype
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
 * Link a pane into another window (a second view of the same pane), or unlink
 * one such view. Modeled on join-pane/link-window.
 */

static enum cmd_retval	cmd_link_pane_exec(struct cmd *, struct cmdq_item *);
static enum cmd_retval	cmd_unlink_pane_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_link_pane_entry = {
	.name = "link-pane",
	.alias = "linkp",

	.args = { "bdhvl:s:t:", 0, 0, NULL },
	.usage = "[-bdhv] [-l size] " CMD_SRCDST_PANE_USAGE,

	.source = { 's', CMD_FIND_PANE, CMD_FIND_DEFAULT_MARKED },
	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_link_pane_exec
};

const struct cmd_entry cmd_unlink_pane_entry = {
	.name = "unlink-pane",
	.alias = "unlinkp",

	.args = { "kt:", 0, 0, NULL },
	.usage = "[-k] " CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_unlink_pane_exec
};

static enum cmd_retval
cmd_link_pane_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*current = cmdq_get_current(item);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct cmd_find_state	*source = cmdq_get_source(item);
	struct session		*dst_s = target->s;
	struct winlink		*dst_wl = target->wl;
	struct window		*dst_w = dst_wl->window;
	struct window_pane	*dst_wp = target->wp;
	struct window_pane	*src_wp = source->wp;
	struct panelink		*pl;
	struct layout_cell	*lc;
	enum layout_type	 type;
	int			 flags, size = -1;
	u_int			 curval = 0;
	char			*cause = NULL;

	if (panelink_find_by_pane(&dst_w->panes, src_wp) != NULL) {
		cmdq_error(item, "pane already linked in target window");
		return (CMD_RETURN_ERROR);
	}

	server_unzoom_window(dst_w);

	type = LAYOUT_TOPBOTTOM;
	if (args_has(args, 'h'))
		type = LAYOUT_LEFTRIGHT;

	if (args_has(args, 'l')) {
		if (type == LAYOUT_TOPBOTTOM)
			curval = dst_wp->sy;
		else
			curval = dst_wp->sx;
		size = args_percentage_and_expand(args, 'l', 0, INT_MAX, curval,
		    item, &cause);
		if (cause != NULL) {
			cmdq_error(item, "size %s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}
	}

	flags = 0;
	if (args_has(args, 'b'))
		flags |= SPAWN_BEFORE;

	lc = layout_split_pane(dst_wp, type, size, flags);
	if (lc == NULL) {
		cmdq_error(item, "create pane failed: pane too small");
		return (CMD_RETURN_ERROR);
	}

	/*
	 * Bind the new cell to the source pane as a second view. The cell
	 * points at this view's panelink (lc->pl); the pane's home layout cell
	 * is a different cell on a different panelink and is left untouched.
	 */
	pl = panelink_add(&dst_w->panes);
	pl->window = dst_w;
	panelink_set_pane(pl, src_wp);
	TAILQ_INSERT_TAIL(&dst_w->z_index, pl, zentry);

	lc->type = LAYOUT_WINDOWPANE;
	TAILQ_INIT(&lc->cells);
	lc->pl = pl;
	pl->layout_cell = lc;

	layout_fix_panes(dst_w, NULL);

	recalculate_sizes();

	server_redraw_window(dst_w);

	if (!args_has(args, 'd')) {
		window_set_active_pane(dst_w, src_wp, 1);
		session_select(dst_s, dst_wl->idx);
		cmd_find_from_session(current, dst_s, 0);
		server_redraw_session(dst_s);
	} else
		server_status_session(dst_s);

	return (CMD_RETURN_NORMAL);
}

static enum cmd_retval
cmd_unlink_pane_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window		*w = target->wl->window;
	struct window_pane	*wp = target->wp;
	struct panelink		*pl;

	pl = panelink_find_by_pane(&w->panes, wp);
	if (pl == NULL) {
		cmdq_error(item, "pane not found in window");
		return (CMD_RETURN_ERROR);
	}

	if (wp->references <= 1 && !args_has(args, 'k')) {
		cmdq_error(item, "last reference to pane, use -k to destroy");
		return (CMD_RETURN_ERROR);
	}

	if (pl->window == wp->window) {
		cmdq_error(item, "cannot unlink a pane's home window view "
		    "(use break-pane or kill-pane)");
		return (CMD_RETURN_ERROR);
	}

	server_unzoom_window(w);
	server_client_remove_pane(wp);
	window_lost_pane(w, wp);

	/*
	 * Destroy this view's layout cell. The cell points at this panelink
	 * (lc->pl), so freeing it clears only this view's cell, never the pane's
	 * home layout cell (a different cell on a different panelink).
	 */
	if (pl->layout_cell != NULL) {
		layout_destroy_cell(w, pl->layout_cell, &w->layout_root);
		pl->layout_cell = NULL;
		if (w->layout_root != NULL) {
			layout_fix_offsets(w);
			layout_fix_panes(w, NULL);
		}
		notify_window("window-layout-changed", w);
	}

	TAILQ_REMOVE(&w->z_index, pl, zentry);
	panelink_remove(&w->panes, pl);

	recalculate_sizes();
	server_redraw_window(w);

	return (CMD_RETURN_NORMAL);
}
