#!/usr/bin/env bash
# C++ counterpart of tools/i686-elf-gcc-glib-wrapper.sh — see that
# script's own comment for the full reasoning (Meson's `dependency
# ('threads')` unconditionally appending an unsupported -pthread flag).
set -euo pipefail
args=()
for arg in "$@"; do
	if [ "${arg}" != "-pthread" ]; then
		args+=("${arg}")
	fi
done
exec i686-elf-g++ "${args[@]}"
