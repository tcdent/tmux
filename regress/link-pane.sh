#!/bin/sh

# link-pane and unlink-pane: a single pane referenced from two windows

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
$TMUX kill-server 2>/dev/null

# Two windows; remember window 0's only pane.
$TMUX -f/dev/null new -d -x80 -y24 || exit 1
$TMUX neww -d || exit 1
P=$($TMUX lsp -t:0 -F'#{pane_id}')

# Link it into window 1 as a second view.
$TMUX link-pane -d -s "$P" -t:1 || exit 1

# The pane is now listed in both windows, and window 1 has two panes.
[ "$($TMUX lsp -a -F'#{pane_id}'|grep -c "^$P\$")" = 2 ] || exit 1
[ "$($TMUX lsp -t:1|wc -l)" -eq 2 ] || exit 1

# pane_index is per-view: index 0 in its home window, index 1 in window 1.
[ "$($TMUX display -t:0."$P" -p '#{pane_index}')" = 0 ] || exit 1
[ "$($TMUX display -t:1."$P" -p '#{pane_index}')" = 1 ] || exit 1

# Targeting the linked view by pane id within its window resolves there.
[ "$($TMUX display -t:1."$P" -p '@#{window_id}')" \
    = "$($TMUX display -t:1 -p '@#{window_id}')" ] || exit 1

# Unlinking the home-window view is refused.
$TMUX unlink-pane -t:0 2>/dev/null && exit 1

# Unlinking the linked view (by pane id) leaves the pane alive in window 0.
$TMUX unlink-pane -t:1."$P" || exit 1
[ "$($TMUX lsp -a -F'#{pane_id}'|grep -c "^$P\$")" = 1 ] || exit 1
[ "$($TMUX lsp -t:1|wc -l)" -eq 1 ] || exit 1

# A linked view does not keep the pane alive when its window is killed: the
# pane survives because its home window still references it, and the home
# layout is not corrupted.
$TMUX link-pane -d -s "$P" -t:1 || exit 1
$TMUX kill-window -t:1 || exit 1
[ "$($TMUX lsp -a -F'#{pane_id}'|grep -c "^$P\$")" = 1 ] || exit 1
$TMUX splitw -t:0 -d || exit 1
[ "$($TMUX lsp -t:0|wc -l)" -eq 2 ] || exit 1

# kill-pane destroys a shared pane in every window it is linked into, without
# crashing (regression: killing a linked view dereferenced a stale home
# window). Afterwards the server is still responsive.
NW=$($TMUX neww -dP -F'#{window_id}') || exit 1
$TMUX link-pane -d -s "$P" -t"$NW" || exit 1
[ "$($TMUX lsp -a -F'#{pane_id}'|grep -c "^$P\$")" = 2 ] || exit 1
$TMUX kill-pane -t "$P" || exit 1
[ "$($TMUX lsp -a -F'#{pane_id}'|grep -c "^$P\$")" = 0 ] || exit 1
$TMUX lsw >/dev/null 2>&1 || exit 1

# When a shared pane's process exits it is destroyed in every window, without
# crashing (regression: the exit was handled per window, freeing the pane then
# dereferencing it again for the next window).
$TMUX splitw -d || exit 1
R=$($TMUX lsp -F'#{pane_id}' | tail -1)
NW2=$($TMUX neww -dP -F'#{window_id}') || exit 1
$TMUX link-pane -d -s "$R" -t"$NW2" || exit 1
[ "$($TMUX lsp -a -F'#{pane_id}'|grep -c "^$R\$")" = 2 ] || exit 1
$TMUX respawn-pane -k -t "$R" true || exit 1
sleep 1
[ "$($TMUX lsp -a -F'#{pane_id}'|grep -c "^$R\$")" = 0 ] || exit 1
$TMUX lsw >/dev/null 2>&1 || exit 1

# A pane can be linked into a window in another session and addressed there by
# id, even though that is not its home session.
$TMUX kill-server 2>/dev/null
$TMUX -f/dev/null new -d -s one -x80 -y24 || exit 1
$TMUX new -d -s two -x80 -y24 || exit 1
Q=$($TMUX lsp -t one: -F'#{pane_id}')
$TMUX link-pane -d -s "$Q" -t two: || exit 1
[ "$($TMUX display -t two:."$Q" -p '#{session_name}')" = two ] || exit 1
[ "$($TMUX display -t one:."$Q" -p '#{session_name}')" = one ] || exit 1

$TMUX kill-server 2>/dev/null
exit 0
