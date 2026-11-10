#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Verify every FIT configuration against the target's trusted control DTB.
set -eu
[ "$#" -eq 5 ] || { echo "Usage: $0 FIT TOPDIR KEY_DTB CLASSICAL_KEY PQC_KEY" >&2; exit 1; }
fit=$1
base=$2
trust=$3
classical=$4
pqc=$5
case "$trust" in
	/*) ;;
	?*) trust="$base/$trust" ;;
	*) echo "FIT signing requires FIT_VERIFY_KEY_DTB" >&2; exit 1 ;;
esac
[ -r "$trust" ] || { echo "Missing trusted U-Boot control DTB: $trust" >&2; exit 1; }
set --
for key in "$classical" "$pqc"; do
	[ -n "$key" ] || continue
	case "$key" in *[!a-zA-Z0-9_-]*) echo "Invalid FIT key name" >&2; exit 1 ;; esac
	set -- "$@" -R "$key"
done
[ "$#" -gt 0 ] || { echo "FIT signing requires a key" >&2; exit 1; }
if [ -n "$classical" ] && [ -n "$pqc" ]; then
	[ "$classical" != "$pqc" ] || { echo "Hybrid FIT requires distinct keys" >&2; exit 1; }
	set -- "$@" -A
fi
fit_check_sign -a -f "$fit" -k "$trust" "$@"
