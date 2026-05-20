# tmux shared panes — design, rooted in the current tree

A patch series to let a single pane be referenced from multiple windows (and
therefore multiple sessions), modeled after the existing `winlink` pattern that
lets a single window be referenced from multiple sessions.

Goal: daily use for the author, and an upstream-quality patch series at the end.

---

## Prototype status (working)

A functional prototype is committed on this branch. `link-pane` puts a pane into
a second window; `unlink-pane [-k]` removes a view. Every step builds clean
(`-Wall -W -Wshadow -Wmissing-prototypes ...`, zero warnings) and is
smoke-tested. Commits:

- **Step 1** — `struct panelink`, refcount, tier-1 helpers (dead code).
- **Step 2** — per-window panelink list maintained in lockstep; destroy via
  refcount.
- **Step 3a/3b/3c** — the three per-window pane lists migrated to panelinks:
  visit stack (`last_panelinks`), z-index (`z_index`), and the positional list
  (`panes`). `entry`/`sentry`/`zentry` now live on the panelink; `w->panes` is
  the single positional panelink list (the Step-2 parallel `w->panelinks` was
  folded into it).
- **Step 3d** — `layout_fix_panes` selects the per-view cell.
- **Commands** — `link-pane` / `unlink-pane`.
- Plus a **pre-existing upstream bugfix**: `layout_assign` flagged tiled panes
  floating on layout restore.

### Prototype shortcuts (deliberate, deviate from the full design)

These keep the diff tractable and are the known gap to "upstream-quality":

1. **`wp->window` is kept** as a maintained back-pointer (the full design drops
   it). The ~144 readers therefore resolve to the pane's *home* window, which is
   why `#{pane_index}` and other per-view formats are wrong for a linked view
   (needs `cmd_find_best_window_with_pane`, design §8 item 2).
2. **`w->active` is kept as `struct window_pane *`** (design §4 says
   `panelink *`). Fine while no pane is linked twice into one window (no `-f`).
3. **`layout_cell` stays on `window_pane`** for a pane's home window; the
   per-view cell for a *linked* window lives on the panelink
   (`pl->layout_cell`), selected in `layout_fix_panes`. The full design moves
   `layout_cell`→panelink and `lc->wp`→`lc->pl` everywhere; deferred.
4. **`link-pane` binds the linked cell via `lc->wp = src_wp`** while leaving
   `src_wp->layout_cell` (home) alone. `unlink-pane` nulls `lc->wp` before
   freeing the cell so the home layout is not corrupted. **Caveat:** destroying
   a window that still holds a *linked* view can clobber the home pane's
   `layout_cell` (the `lc->wp`↔`wp->layout_cell` round-trip is intentionally
   broken). Unlink before closing such a window. The real fix is shortcut 3.

### Known limitations / still to do

- **`pane-size` negotiation not implemented.** A pane has one grid; whichever
  window's `layout_fix_panes` ran last sets its size. Two windows of different
  sizes fight; single-client / one-window-visible is fine. This is the §3
  `resize.c`-mirror work.
- **Per-view formats** (`pane_index`, etc.) resolve via the home window
  (shortcut 1).
- **Not visually verified.** Behaviour confirmed headlessly (list-panes shows
  the pane in two windows with a real split layout; unlink/refcount correct). A
  real attached client has not been driven in this environment.

> **Verification convention.** Every claim about tmux internals carries a
> `file:line` against **this** tree (tmux master at tag `3.6b`, *including the
> floating-panes work* — commits `ce24b92`, `572e26d`). A section marked
> **Verified ✓** has been read in source. **Unverified ⚠** means it is an
> inference not yet grounded. This document supersedes an earlier draft whose
> line numbers predate floating panes and whose data model omits the
> floating-pane fields entirely.

---

## 0. Why this document exists

An earlier audit produced a detailed plan, but it was written against a tree
*before floating panes landed*. Re-verifying every claim against the current
source turned up three structural additions the old plan never saw, and one
design problem it never confronted:

1. **`zentry` + `w->z_index`** — a fourth intrusive per-pane list link and a
   third per-window list, holding floating-pane stacking order.
2. **`PANE_FLOATING`** — a per-view layout property, swapped by `swap-pane`.
3. **`tree_entry` + global `all_window_panes`** — a global id-keyed RB tree of
   panes.
4. **The geometry/size problem** — a pane's size is dictated by its container
   window's layout, but a pane owns exactly one screen grid and one PTY. A
   winlink shares a whole *window* (self-consistent geometry); a panelink would
   share a *sub-component* whose size two windows can disagree on. This is the
   central challenge, analysed in §3.

Everything below is re-grounded.

---

## 1. The precedent: winlink

**Verified ✓** — `tmux.h:1411-1428`, `window.c:166-255`, `session.c:324-372`,
`server-fn.c:248-311`.

A `winlink` is a join object — `{idx, session, window, flags}` plus three
intrusive links: an RB entry on `session->windows` keyed by `idx`
(`tmux.h:1423`), a TAILQ `wentry` on `window->winlinks` so a window knows which
sessions view it (`tmux.h:1424`, `window` carries `references` + `winlinks` at
`tmux.h:1403-1404`), and a TAILQ `sentry` on the session visit stack
(`tmux.h:1425`). No back-pointer to a "primary" session — windows are genuinely
shared.

The three-tier helper layering this series mirrors:

- **Tier 1 (data ops), `window.c`:** `winlink_add` @166, `winlink_set_window`
  @184 (bumps `w->references`), `winlink_remove` @196 (drops the ref;
  `window_remove_ref` @396 destroys at zero), plus `winlink_find_by_*` /
  `_next` / `_previous`. **Verified ✓.**
- **Tier 2 (session wrappers), `session.c`:** `session_attach` @324 fires
  `window-linked` @334; `session_detach` @342 fires `window-unlinked` @350;
  `session_has` @363 tests membership. **Verified ✓.**
- **Tier 3 (server wrappers), `server-fn.c`:** `server_link_window` @248
  (collision/`-k` handling), `server_unlink_window` @305. **Verified ✓.**

This template is **untouched by floating panes** — good news, since the whole
series mirrors it.

---

## 2. Ground truth: what a `window_pane` is *today*

**Verified ✓** — `struct window_pane` at `tmux.h:1248-1343`.

A pane is a single PTY + child process + one screen grid, plus a pile of state.
The sharing design hinges on classifying every field as **per-pane** (shared
content/process — stays on `window_pane`) or **per-view** (an attribute of one
window's view of the pane — moves to the `panelink`).

### Per-pane (stays on `window_pane`)

The PTY/process and content: `id`, `active_point`, `argc/argv/shell/cwd`,
`pid/tty/status/dead_time`, `fd/event`, `offset/base_offset`, `resize_queue`,
timers, `ictx` (input parser), `screen/base` (**the grid**), `status_screen`,
`modes` (copy-mode etc.), `searchstr/searchregex`, `palette`, `cached_gc`,
pipe state, `control_bg/fg`, `scrollbar_style`. Content-/process-level flags:
`PANE_REDRAW`, `PANE_DROP`, `PANE_FOCUSED`, `PANE_INPUTOFF`, `PANE_CHANGED`,
`PANE_EXITED`, `PANE_EMPTY`, `PANE_STATUS*`, `PANE_STYLECHANGED`,
`PANE_THEMECHANGED`, `PANE_UNSEENCHANGES`.

Plus the **global** id-keyed RB membership:
`RB_ENTRY(window_pane) tree_entry` (`tmux.h:1342`) on `all_window_panes`
(`window.c:59,78`; `RB_INSERT` in `window_pane_create` @`window.c:1008`,
`RB_REMOVE` in `window_pane_destroy` @`window.c:1081`). **Verified ✓.** This is
keyed by pane id via `window_pane_cmp`, is global not per-window, and therefore
survives sharing unchanged — it stays on the pane and is dropped exactly once,
at true destroy (refcount zero).

### Per-view (moves to `panelink`)

| Field / flag | Today on `window_pane` | Per-window list it links | Source |
|---|---|---|---|
| `struct window *window` | `tmux.h:1252` | — (the back-pointer itself) | ✓ |
| `layout_cell`, `saved_layout_cell` | `tmux.h:1255-1256` | — (per-view layout) | ✓ |
| `TAILQ_ENTRY entry` | `tmux.h:1339` | `w->panes` (positional order) | ✓ |
| `TAILQ_ENTRY sentry` | `tmux.h:1340` | `w->last_panes` (visit stack) | ✓ |
| `TAILQ_ENTRY zentry` | `tmux.h:1341` | `w->z_index` (floating z-order) | ✓ **new** |
| `PANE_VISITED` (`0x8`) | `tmux.h:1268` | tracks `last_panes` membership | ✓ |
| `PANE_ZOOMED` (`0x10`) | `tmux.h:1269` | tracks per-window zoom layout | ✓ |
| `PANE_FLOATING` (`0x20`) | `tmux.h:1270` | tracks `z_index` / floating layout | ✓ **new** |
| geometry `xoff/yoff/sx/sy` | `tmux.h:1258-1262` | derived from `layout_cell` — see §3 | ✓ |

`PANE_VISITED` is set/cleared in lockstep with `sentry` by
`window_pane_stack_push`/`_remove` (`window.c:1682-1699`). **Verified ✓.**

`PANE_FLOATING` is per-view because floating-ness is a layout attribute:
`swap-pane` swaps it between two panes (`cmd-swap-pane.c:107-110`) and refuses
to swap two floaters (`cmd-swap-pane.c:82-86`). **Verified ✓.** A shared pane
could float in window A and tile in window B.

**`PANE_ZOOMED` (`0x10`) is per-view. Verified ✓** — `window_zoom`
(`window.c:695-720`) swaps `w->layout_root` ↔ `w->saved_layout_root` and
rewrites every pane's `layout_cell`/`saved_layout_cell` to show only the zoomed
pane; `window_unzoom` (@723) restores them. Zoom is therefore a transformation
of the *window's layout*, which is per-view. Since `layout_cell` /
`saved_layout_cell` migrate to the panelink, `PANE_ZOOMED` migrates with them as
`PANELINK_ZOOMED`. A pane can be zoomed in window A and tiled in window B.

### Transient render scratch (recomputed, no migration needed)

`struct visible_ranges r` (`tmux.h:1337`) — the non-occluded spans of a pane,
rebuilt every redraw (`screen-redraw.c:1159-1160`, consumed via
`tty_check_overlay_range`). `sb_slider_y/h` scrollbar slider geometry. These are
recomputed per render; in a shared world they are recomputed per (pane, view)
during that view's redraw and need no persistent home. **Verified ✓** for `r`'s
recompute-per-redraw nature.

### The three per-window pane lists

**Verified ✓** — `struct window` at `tmux.h:1349-1407`:

```c
struct window_pane  *active;       /* per-window: fine as-is */
struct window_panes  last_panes;   /* visit stack, via sentry */
struct window_panes  z_index;      /* floating z-order, via zentry  <- new */
struct window_panes  panes;        /* positional order, via entry */
```

All three become lists of `panelink`. `active` stays a pointer (it is already
per-window). **Note:** pane order is *positional*, not RB-keyed —
`window_pane_at_index` (`window.c:840-852`) walks `&w->panes` counting from
`pane-base-index`. So panelinks are **TAILQ, not RB** (an earlier draft got this
wrong). This preserves `select-pane -t 3` semantics. No `idx` field on the
panelink.

---

## 3. The central challenge: a pane has one grid, windows disagree on size

**Verified ✓** — `layout_fix_panes` at `layout.c:358-405`.

A pane's geometry is **derived from its layout cell**, then pushed into the one
grid:

```c
TAILQ_FOREACH(wp, &w->panes, entry) {
    if ((lc = wp->layout_cell) == NULL || wp == skip) continue;
    wp->xoff = lc->xoff;
    wp->yoff = lc->yoff;
    ...
    window_pane_resize(wp, sx, sy);   /* resizes wp->base, the grid */
}
```

`window_pane_resize` resizes `wp->base` (the single screen grid) and the PTY is
told via `TIOCSWINSZ`. There is **one** grid and **one** child process per pane.

This is where pane-sharing diverges from window-sharing:

- A **winlink** shares a whole *window*. A window owns one size; multiple
  clients/sessions viewing it negotiate that one size through the
  `window-size` option (`WINDOW_SIZE_LARGEST/SMALLEST/MANUAL/LATEST`,
  `tmux.h:1431-1434`). Internal pane sizes are therefore globally consistent —
  no conflict.
- A **panelink** would share a *sub-component*. Windows `W1` and `W2` can have
  different sizes and different layouts, so the pane's cell is e.g. 80×24 in
  `W1` and 40×10 in `W2`. But the pane has one grid and one process. **The
  conflict is fundamental, not incidental.**

This is the same class of problem tmux already solved one level up, for windows.
**The design is to duplicate the `window-size` machinery (`resize.c`) one level
down**, not to invent something new.

### 3.1 How `window-size` works today (the pattern to mirror)

**Verified ✓** — `resize.c:99-460`, `options-table.c:1521-1531`,
`layout.c:364-405`.

The chain is **clients → negotiate → window size → layout → cells → pane grid**:

1. `recalculate_sizes()` (@`resize.c:420`) is the global driver, fired on every
   relevant change (attach/detach, client resize, link/unlink, layout change —
   ~25 call sites). `recalculate_sizes_now` (@426) walks **every window**
   (`RB_FOREACH(w, windows, &windows)`, @458) and calls `recalculate_size(w)`.
2. `recalculate_size(w)` (@353) reads the `window-size` option
   (largest/smallest/latest/manual, @371) and `aggressive-resize` (@372), then
   calls `clients_calculate_size(...)` (@375).
3. `clients_calculate_size` (@114) is the negotiation core. It **iterates all
   clients** (`TAILQ_FOREACH(loop, &clients, entry)`, @152), skips those not
   viewing the window (`session_has` via a `skip_client` callback) or flagged
   ignore (`ignore_client_size`, @68), and folds each candidate's size
   (`loop->tty.sx`, `tty.sy - status_line_size`, @186-188) into a running
   min (smallest), max (largest), the `w->latest` client (latest, @167), or
   `w->manual_sx/sy` (manual, @130).
4. The result is applied by `resize_window(w, sx, sy)` (@26): `layout_resize`
   redistributes across the split tree → each cell gets `sx/sy/xoff/yoff` →
   `layout_fix_panes` (`layout.c:364`) pushes the cell rectangle into each
   pane's grid via `window_pane_resize`, which resizes `wp->base` and
   `TIOCSWINSZ`-es the PTY.

The unit of negotiation is the **window**. Panes never negotiate — they are told
by the layout. A pane has exactly one grid because it always belonged to exactly
one window.

### 3.2 `pane-size`: the same machinery, one level down

The mapping is mechanical:

| `window-size` (window unit) | source | `pane-size` (pane unit) |
|---|---|---|
| candidates iterated | `&clients`, filtered by `session_has(c->session, w)` | `wp->panelinks` — the windows viewing the pane |
| each candidate's size | `c->tty.sx`, `tty.sy - status` (`resize.c:186`) | `pl->layout_cell->sx/sy` — the cell that window's layout assigned |
| option (choice) | `window-size` (`options-table.c:1521`) | new `pane-size`, same 4 choices, `OPTIONS_TABLE_PANE` scope |
| "latest" pointer | `w->latest` (client, `tmux.h:1351`) | new `wp->latest` (panelink — the last-focused view) |
| manual size | `w->manual_sx/sy`, set by `resize-window` | `wp` manual size, set by `resize-pane` |
| negotiation fn | `clients_calculate_size` (@114) | new `panelinks_calculate_size` |
| per-unit driver | `recalculate_size(w)` (@353) | new `recalculate_pane_size(wp)` |
| global driver | `recalculate_sizes` walks windows (@458) | extend it to also walk panes |
| apply | `resize_window`→`layout_resize`→`layout_fix_panes`→`window_pane_resize` | `window_pane_resize(wp, sx, sy)` directly (grid + PTY) |

**The one architectural change:** `layout_fix_panes` (`layout.c:364-405`)
currently *fuses* two things — it sets the cell's view rectangle
(`wp->xoff/yoff` = `lc->xoff/yoff`) **and** resizes the grid (`window_pane_resize`)
in the same loop. Under sharing these must decouple:

- **View rectangle** (where the pane is drawn in this window): per-panelink,
  already carried by `pl->layout_cell` (`sx/sy/xoff/yoff`). This is set by
  phase 1 (window layout), independent of the grid.
- **Grid size** (the pane's content, the PTY size): negotiated in a new phase 2
  from all panelinks' cells, then applied once via `window_pane_resize`.

So `recalculate_sizes_now` gains a second pass after the window loop:
*phase 1* lays out every window's cells (existing); *phase 2* walks every pane
and negotiates its grid from its panelinks' cells. There is **no chicken-and-egg**
— cell geometry comes from the split tree + window size, never from the pane's
grid (`layout_resize` reads only `PANE_MINIMUM` constants), so phase 1 does not
depend on phase 2's output.

### 3.3 Rendering when cell ≠ grid

Once the grid can differ from a view's cell, the renderer clips or pads:
- **cell smaller than grid** (e.g. `pane-size largest`, this view is the small
  one) → draw a sub-rectangle of the grid;
- **cell larger than grid** (e.g. `pane-size smallest`, this view is the big
  one) → draw the grid and pad the remainder.

This is exactly the multi-client-same-window tradeoff today, and the primitive
already exists: floating panes added `struct visible_ranges` +
`tty_check_overlay_range` (`tty.c:1456`, `tty-draw.c:85`) precisely to paint a
sub-rectangle of a pane into a view. Floating panes therefore both **complicate**
sharing (more per-view state) and **enable** it (the clipping primitive that did
not exist when the original audit was written).

### 3.4 Default and degeneracy

**Verified ✓** — `window-size` defaults to `latest` (`options-table.c:1525`,
`WINDOW_SIZE_LATEST`). `pane-size` mirrors this: **default `latest`**, meaning
the grid follows the most-recently-focused view. With a single panelink,
"latest/largest/smallest of one cell" is that cell, and `window_pane_resize` to
the cell is exactly what `layout_fix_panes` does today — so the negotiation is a
**no-op in the unshared case**, preserving the "zero behaviour change" guarantee
of Steps 1–5. The feature only bites once a second panelink exists (Step 6).

`aggressive-resize` has a natural analog ("only size to windows where this pane
is active") but is **deferred** — see §8 ambiguity 9.

---

## 4. The data model

A `panelink` joins a pane to a window the same way a winlink joins a window to a
session.

```c
struct panelink {
    struct window        *window;
    struct window_pane   *pane;
    int                   flags;        /* PANELINK_VISITED/ZOOMED/FLOATING */

    /* per-view layout state, migrated off window_pane */
    struct layout_cell   *layout_cell;
    struct layout_cell   *saved_layout_cell;

    TAILQ_ENTRY(panelink) entry;    /* on window->panelinks  (positional)  */
    TAILQ_ENTRY(panelink) sentry;   /* on window->last_panelinks (visit)    */
    TAILQ_ENTRY(panelink) zentry;   /* on window->z_index_panelinks (float) */
    TAILQ_ENTRY(panelink) wentry;   /* on pane->panelinks (fan-out)         */
};
TAILQ_HEAD(panelinks, panelink);
```

Changes to `struct window_pane` (`tmux.h:1248`):
- **Remove** `struct window *window` → `TAILQ_HEAD(, panelink) panelinks`
  (the fan-out: which windows view this pane) + `u_int references`.
- **Remove** `layout_cell`, `saved_layout_cell` → migrate to `panelink`.
- **Remove** `entry`, `sentry`, **and `zentry`** → migrate to `panelink`.
- **Remove** flags `PANE_VISITED`, `PANE_ZOOMED`, and `PANE_FLOATING` → become
  `panelink` flags (`PANELINK_VISITED/ZOOMED/FLOATING`).
- **Add** `void *latest` (the last-focused panelink, mirroring `w->latest`,
  `tmux.h:1351`) and `u_int manual_sx/manual_sy` (mirroring `w->manual_sx/sy`,
  `tmux.h:1375-1376`) for the `pane-size` negotiation (§3.2).
- **Keep** `tree_entry` (global id RB), the grid, PTY, and all per-pane state.
- Geometry `xoff/yoff/sx/sy` and the grid: see §3 — the grid (`wp->base`) stays
  the single negotiated content size; the panelink's `layout_cell` is the
  per-view view rectangle. Decoupling these in `layout_fix_panes` is the one
  architectural change (§3.2).

Changes to `struct window` (`tmux.h:1349`):
- `panes` → `panelinks`, `last_panes` → `last_panelinks`,
  `z_index` → `z_index_panelinks` (all TAILQ of `panelink`).
- **`active` becomes `struct panelink *`** (not `window_pane *`). This mirrors
  the winlink precedent — a session's current window is `s->curw`, a
  `winlink *` (the join object), not a `window *`. It is also mechanically
  forced: `window_lost_pane` recovers active via
  `TAILQ_FIRST(&w->last_panes)` / `TAILQ_PREV(... entry)` (`window.c:815-819`),
  which after migration yield panelinks. `w->active->pane` reaches the shared
  state. As a bonus this disambiguates same-window duplicate links (amb. 14):
  two panelinks of one pane in one window, `active` names exactly one.

Changes to `struct layout_cell` (`tmux.h:1470`):
- `struct window_pane *wp` → `struct panelink *pl`. `lc->pl->pane` reaches the
  shared state, `lc->pl->window` reaches the container. (`LAYOUT_FLOATING`
  cells, `tmux.h:1462`, are unaffected structurally — they hold child cells the
  same way.)

---

## 5. Helper layering (mirrors winlink)

**Tier 1 — `window.c` data ops** (copy-rename of `winlink_*`):
`panelink_add`, `panelink_set_pane` (bumps `wp->references`), `panelink_remove`
(drops it; `window_pane_remove_ref` destroys at zero), `panelink_find_by_pane`,
`panelink_find_by_pane_id`, `panelink_next`, `panelink_previous`,
`window_pane_add_ref` / `window_pane_remove_ref` (mirror `window_add_ref` /
`window_remove_ref` @`window.c:389-402`, with `__func__` log_debug).

**Tier 2 — `window.c` wrappers:** `window_attach_pane` / `window_detach_pane`
replacing `window_add_pane` (@771) / `window_remove_pane` (@831) /
`window_lost_pane` (@806). Fire `pane-linked` / `pane-unlinked` notifications
mirroring `window-linked` / `window-unlinked` (`session.c:334,350`).

**Tier 3 — `server-fn.c`:** `server_link_pane` / `server_unlink_pane` mirroring
`server_link_window` / `server_unlink_window` (@248/305). Collision + `-k`,
marked-pane fixup (`marked_pane` is a `cmd_find_state` at `server.c:51`, set
`s/wl/w/wp` at `server.c:70-75` — no struct change, just re-point `wl/w` on
relink), redraw trigger.

---

## 6. Commands

**Verified ✓** — `cmd-move-window.c` shares one `exec` between
`cmd_move_window_entry` and `cmd_link_window_entry`, dispatched by
`cmd_get_entry`. `cmd-join-pane.c` already moves a pane between windows (it just
always destroys the source link).

- `link-pane -s src -t dst-window[.idx] [-f]` — add a panelink in `dst` pointing
  at `src`'s pane. Refuse same-window by default (mirror `cmd-join-pane.c:92`
  `src_wp == dst_wp` check); `-f` allows multiple cells in one window viewing
  one pane.
- `unlink-pane -t target [-k]` — remove this view. Refuse to remove the last
  reference unless `-k` (then destroy). Mirrors `unlink-window`.
- `kill-pane` — **unchanged semantics**: destroy the pane completely, now via
  refcount cascade across all panelinks. `-a` is taken (kill *other* panes,
  `cmd-kill-pane.c:52`), so it is **not** repurposed.
- `break-pane` — generalizes: today it removes from `w->panes`/`w->z_index` and
  attaches to a new window (`cmd-break-pane.c:98-108`); becomes "remove this
  panelink, create a new window with a new panelink." Other views survive.
- `swap-pane` — swaps **panelinks** not panes: positions in the three TAILQs,
  `layout_cell` pointers, and the `PANELINK_FLOATING` flag (today it pointer-
  swaps `wp->window`, `layout_cell`, and `PANE_FLOATING` at
  `cmd-swap-pane.c:91-117`). The float-vs-float guard (`82-86`) is preserved.
  Cleaner than today: panes do not move, only their views' slots.

---

## 7. The `wp->window` audit (recounted on this tree)

**Verified ✓** by grep on the current tree:

| Metric | This tree |
|---|---|
| `wp->window` readers | **144**, across **24** files |
| `TAILQ_FOREACH(&w->panes)` iterations | **43** (73 total `&w->panes` refs incl. mutators) |
| `wp->layout_cell` / `saved_layout_cell` | **38** |
| `&w->z_index` / `zentry` sites | **~20** (new — `window.c`, `screen-redraw.c`, `layout.c`, `layout-custom.c`, `cmd-{break,join}-pane.c`) |

Per-file `wp->window` distribution: `window.c` 26, `window-copy.c` 17,
`layout.c` 15, `input.c` 14, `cmd-select-pane.c` 14, `screen-write.c` 8,
`screen-redraw.c` 6, `format.c` 6, `cmd-find.c` 6, others ≤4.

### Buckets

- **(a) Pure substitution** — caller already has a `cmd_find_state` / `winlink`
  / `window` and used `wp->window` as a shortcut. *Verified examples:*
  `cmd-select-pane.c:207` `server_redraw_window_borders(wp->window)` →
  `target.w`; `screen-write.c:112` already does `w = wp->window;`.
- **(b/c1) Fan-out on PTY events** — output→activity, BEL→bell, focus. Replace
  the pointer with a walk over `wp->panelinks`. The mechanism exists:
  `window_pane_update_focus` (`window.c:488`) already walks `&clients` and
  compares `c->session->curw->window == wp->window`; that compare becomes
  "client views this pane through any panelink." *Verified example:*
  `input.c` `window_update_activity(wp->window)` →
  `TAILQ_FOREACH(pl, &wp->panelinks, wentry) window_update_activity(pl->window);`
- **(c2) Per-view question masquerading as per-pane** — the call site has view
  context it was not using. *Verified:* `format_tree` carries independent
  `{c,s,wl,w,wp}`; `cmd_find_state` the same shape (`server.c:51`).

  | Site | Today | After |
  |---|---|---|
  | `format.c:2027` | `ft->wp == ft->wp->window->active` | `ft->w != NULL && ft->wp == ft->w->active` |
  | `window-copy.c:458` (+~9 siblings) | `wp->window->options` | `w->options` from caller |
  | `layout.c:999,1114` | `wp->window->layout_root` | `pl->window->layout_root` |
  | `screen-write.c:142` | `c->session->curw->window != wp->window` | `panelink_find_by_pane(&curw->window->panelinks, wp) == NULL` |

- **(d) Signature ripple** — functions that must take an explicit `window`:
  `window_pane_visible`, the `layout_*` family, `screen_redraw_pane_border`,
  `screen_write_alternateon`/`off` (`screen-write.c:2476-2501`),
  `window_copy_*`, `mode_tree_*`, `window_clock_draw_screen`. Plus the special
  case `screen_write_initctx` (`screen-write.c:217`,
  `ctx->wp != ctx->wp->window->active`) which runs *before* per-client fan-out
  and needs a helper `window_pane_is_active_anywhere(wp)` walking panelinks
  (in the single-link case it equals today's check).

### `cmd_find` resolution

**Verified ✓** — `cmd-find.c`: best-session via `session_has` loop (@186) +
`cmd_find_session_better` (@134), then `cmd_find_best_winlink_with_window`
(@209). The pane analog adds one level: collect candidate windows by walking
`wp->panelinks`, then run the existing logic. New helper
`cmd_find_best_window_with_pane(fs)` slots in symmetrically. **Three** call
sites need it: `cmd_find_from_pane` (@807), `notify_add` (@179; replace
`wp->window->id`/`name` at `notify.c:213,215` with `ne->fs.w->...`), and
`format_defaults_pane` (@`format.c:5923-5928`, which backfills `ft->w` via
`format_defaults_window(ft, wp->window)`).

---

## 8. Ambiguities and decisions

Carrying forward the verified ones and adding the floating-pane and geometry
ones surfaced by this pass.

1. **kill-pane vs unlink-pane** — `kill-pane` = destroy (refcount cascade),
   `unlink-pane` = per-view remove. **Verified ✓** (`cmd-kill-pane.c:52` shows
   `-a` is taken).
2. **`cmd_find` of a pane in multiple windows** — `cmd_find_best_window_with_pane`
   then existing best-session/winlink. **Verified ✓** (§7).
3. **Hooks** — signature unchanged; non-interactive triggers build state via
   `cmd_find_from_pane`. Three internal sites get the helper from #2.
   **Verified ✓**.
4. **`pane_index` / `list-panes -a`** — resolve via `ft->w`; iterate
   sessions→winlinks→panelinks (iterate join objects, not shared objects).
   **Verified ✓** (`cmd-list-panes.c` walks `RB_FOREACH(wl, ...)` then expands).
5. **Window-scoped options for a shared pane** — `wp->options->parent` can only
   point to one window (`window_pane_create` @`window.c:1004`,
   `options_set_parent` on move @`cmd-join-pane.c:152`). Decision: **bypass the
   parent chain** for window-scoped lookups — use `w->options` from caller
   context (e.g. the ~10 `wp->window->options` mode-keys/wrap lookups in
   `window-copy.c`). Needs a sub-audit of which keys are window- vs pane-scoped.
   **Verified ✓** the chain re-points on move.
6. **Copy-mode state** — shared, on the pane (`wp->modes`). **Verified ✓.**
7. **respawn-pane** — affects all views (one process). **Verified ✓.**
8. **`PANE_ZOOMED` per-view.** Resolved → `PANELINK_ZOOMED`. **Verified ✓** —
   `window_zoom`/`window_unzoom` (`window.c:695-746`) implement zoom as a
   per-window layout transformation (swap `layout_root`, rewrite every pane's
   `layout_cell`/`saved_layout_cell`), so the flag follows the per-view layout
   fields onto the panelink. See §2.
9. **Geometry / pane size** — the §3 problem, solved by **duplicating the
   `window-size` machinery** (`resize.c:99-460`) one level down: a `pane-size`
   option (largest/smallest/latest/manual, default `latest`), a
   `panelinks_calculate_size` mirroring `clients_calculate_size`, a
   `recalculate_pane_size` mirroring `recalculate_size`, `wp->latest` mirroring
   `w->latest`, and a second pass in `recalculate_sizes_now`. The one
   architectural change is decoupling view-rectangle from grid-size in
   `layout_fix_panes` (§3.2). **The `aggressive-resize` analog** ("only size to
   windows where this pane is active", mirroring `recalculate_size_skip_client`
   @`resize.c:336`) is **deferred** to a later refinement. **Verified ✓** the
   pattern being mirrored; **Unverified ⚠** the prototype.
10. **Floating-ness per-view** — `PANE_FLOATING` → `PANELINK_FLOATING`. A pane
    may float in one window, tile in another. **Verified ✓** that the flag is
    swapped/toggled per layout (`cmd-swap-pane.c:107-110`).
11. **Marked pane** — no struct change (`server.c:51`). **Verified ✓.**
12. **Control mode** — add `%pane-linked @W %P` / `%pane-unlinked @W %P`.
    Backward-compat with clients that drop unknown `%`-notifications.
    **Unverified ⚠** against the *current* iTerm2 tree (the old audit verified
    an older copy; re-confirm before upstream, not a blocker for local use).
13. **Detach-pane** — not added; refcount zero = destroy.
14. **`w->active` is a `panelink *`, not a `window_pane *`.** Resolved (§4) by
    the `s->curw` precedent and forced by `window_lost_pane`. This is the only
    correction to the previous draft's data model (it claimed "active stays
    as-is"). **Verified ✓.**
15. **Same-window duplicate links (`link-pane -f`)** — now *supportable*
    unambiguously thanks to amb. 14, but exposing the flag in v1 is a product
    choice. **Decision:** keep the capability, defer the `-f` flag (refuse
    same-window by default, as `cmd-join-pane.c:92` does). Revisit after
    bake-in.
16. **Open by design** — new ambiguities found by daily driving get appended
    here with a status.

---

## 9. Patch series order

Steps 1–5 are invisible refactors: the panelink count is always exactly 1, so
the new code paths run but degenerate to today's behaviour. Step 6 turns the
feature on. Each step is individually defensible as "modernize this to match the
winlink pattern."

1. **Foundation** — `struct panelink`, `references` + `panelinks` on
   `window_pane`, `TAILQ_INIT(&wp->panelinks)` in `window_pane_create`
   (@`window.c:997`), tier-1 `panelink_*` + `window_pane_{add,remove}_ref`.
   Dead code until Step 2.
2. **Wire in lockstep** — in `window_add_pane` (@771) and the four mutators
   (`window_pane_create`, `cmd-break-pane.c:104`, `cmd-join-pane.c:151`,
   `cmd-swap-pane.c:112/115`), mirror every `wp->window =` **and every
   `entry`/`sentry`/`zentry` TAILQ op** with a panelink op. Invariant:
   `TAILQ_FIRST(&wp->panelinks)->window == wp->window`, count == 1.
   *(Larger than the old plan said — the lockstep now spans three lists.)*
3. **Field migration (the big mechanical diff)** — move `entry`, `sentry`,
   **`zentry`**, `layout_cell`, `saved_layout_cell` to `panelink`;
   `PANE_VISITED`→`PANELINK_VISITED`, `PANE_FLOATING`→`PANELINK_FLOATING`;
   `w->panes/last_panes/z_index` → panelink lists; migrate the 43 iterations,
   38 `layout_cell` refs, ~20 `z_index` sites, and
   `window_pane_stack_push`/`_remove`; rewrite `swap-pane` as a panelink swap.
4. **Bucket (a) substitutions + the `cmd_find_best_window_with_pane` helper**
   (3 sites). `wp->window` still exists.
5. **Buckets (c)/(d): fan-out loops + signature changes**, the
   `window_pane_is_active_anywhere` helper, the options sub-audit (#5), then
   **drop `wp->window`** and route destroy through `window_pane_remove_ref`.
6. **Size negotiation machinery (still a no-op while count == 1)** — duplicate
   the `window-size` pattern per §3.2: add the `pane-size` option
   (`options-table.c`), `wp->latest` + `wp->manual_sx/sy`,
   `panelinks_calculate_size` (mirror `clients_calculate_size`),
   `recalculate_pane_size` (mirror `recalculate_size`), and a second pass over
   panes in `recalculate_sizes_now`. **Decouple `layout_fix_panes`** so it sets
   the per-view rectangle from the cell but defers grid sizing to the new pass.
   With one panelink, "latest of one cell" == today's `window_pane_resize`, so
   **still zero behaviour change** — and now independently bisectable.
7. **Commands** — `link-pane`/`unlink-pane`,
   `server_link_pane`/`server_unlink_pane`, `kill-pane` refcount cascade,
   same-window `-f` guard. **First behaviour change:** panelink count can exceed
   1, the §3 negotiation and the fan-out loops actually engage. Daily-drive,
   append findings to §8.
8. **Polish** — control-mode notifications, new formats
   (`#{pane_link_count}`, `#{pane_linked}`), `pane-size`/`aggressive-resize`
   analog refinement, man pages.
9. **Upstream prep** — rebase to ~8 commits, mail tmux-users@, coordinate the
   iTerm2 UX question separately.

---

## 10. What is verified vs what needs a prototype

**Verified against this tree:** the winlink template (§1), the per-pane/per-view
field classification including `PANE_ZOOMED` (§2), the geometry-derivation
mechanism *and the entire `window-size` machinery `pane-size` mirrors*
(§3, `resize.c:99-460`), the `w->active`-as-`panelink *` correction (§4), the
audit counts and buckets (§7), and ambiguities 1–8, 10, 11, 13–15.

**Needs prototyping / re-grounding:** the clip/pad **rendering** UX when grid ≠
cell (§3.3 — the biggest genuine unknown, only answerable by running a shared
pane at two sizes), the `pane-size` *implementation* + `layout_fix_panes`
decoupling (§3.2), the window-scoped options sub-audit (amb. 5), and the
current-tree iTerm2 control-mode check (amb. 12).

Steps 1–3 are safe to start now; nothing in them depends on the open questions.
Step 6 (size machinery) is also a provable no-op while the panelink count is 1,
so it can land and be validated before any command turns the feature on.
