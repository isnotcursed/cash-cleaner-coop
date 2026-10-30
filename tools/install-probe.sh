#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    printf 'usage: %s /path/to/ue4ss\n' "$0" >&2
    exit 64
fi

target=$1
source_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)/ue4ss/Mods/CashCleanerCoopProbe

if [[ ! -d "$target/Mods" ]]; then
    printf 'error: %s/Mods does not exist\n' "$target" >&2
    exit 66
fi

install -d "$target/Mods/CashCleanerCoopProbe/Scripts"
install -m 0644 "$source_dir/enabled.txt" "$target/Mods/CashCleanerCoopProbe/enabled.txt"
install -m 0644 "$source_dir/Scripts/main.lua" "$target/Mods/CashCleanerCoopProbe/Scripts/main.lua"
printf 'installed CashCleanerCoopProbe into %s/Mods\n' "$target"
