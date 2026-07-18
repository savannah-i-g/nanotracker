#!/usr/bin/env bash
# Stage-boundary backup: tars the native project (build trees excluded)
# into Backups/ beside the workspace. Usage: tools/backup.sh <label>
# The resulting filename must be recorded in Docs/PROGRESS.md.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <label>   (e.g. $0 stage0)" >&2
    exit 2
fi

label="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
output_dir="$(dirname "$script_dir")"
workspace_dir="$(dirname "$output_dir")"
backup_dir="$workspace_dir/Backups"
stamp="$(date +%Y-%m-%d)"
archive="$backup_dir/NanoTracker_${label}_${stamp}.tar.gz"

mkdir -p "$backup_dir"
# third_party is rebuildable via tools/build_libopenmpt.sh and large;
# backups carry the scripts, not the artifacts.
tar -czf "$archive" \
    --exclude='Output/build*' \
    --exclude='Output/third_party' \
    -C "$workspace_dir" Output CLAUDE.md

echo "wrote $archive"
