#!/bin/sh
# Advanced Adoption authenticates through the device's configured SSH service.
[ "$#" = 3 ] && [ "$1" = set-adopt ] || exit 1
exec /usr/bin/python3 -B /usr/lib/be14-ucg/agent.py "$@"
