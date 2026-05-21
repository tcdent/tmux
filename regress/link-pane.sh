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

# Unlinking the home-window view is refused.
$TMUX unlink-pane -t:0 2>/dev/null && exit 1

# Unlinking the linked view leaves the pane alive in window 0.
$TMUX unlink-pane -t:1.1 || exit 1
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

$TMUX kill-server 2>/dev/null
exit 0
