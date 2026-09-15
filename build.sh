#!/usr/bin/env bash
# Rebuilds the hyprwobbly plugin against the installed Hyprland headers.
# Run after a Hyprland version update (the .so must match the Hyprland ABI).
#
# NOTE: swapping the .so on a running session requires unloading the loaded
# plugin, which segfaults older builds (missing removeFunctionHook). If the
# currently loaded plugin is an OLD build, end the Hyprland session instead
# and let the new .so load on login (custom.lua auto-loads it).
set -e
cd "$(dirname "$0")/src"
make clean >/dev/null 2>&1 || true
make all
# CRITICAL: never cp over the loaded .so — cp writes into the same inode and
# the running compositor crashes when its mapped pages change under it. mv
# (rename) gives the install a NEW inode; the old mapping keeps the old
# inode alive and the session survives until the next relog.
mv -f hyprwobbly.so "$HOME/.config/hypr/plugins/hyprwobbly.so"
echo "hyprwobbly.so rebuilt and installed to ~/.config/hypr/plugins/"