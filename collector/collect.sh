#!/bin/sh
# Save even failed status/control attempts. No disk reads or trace sessions.
set -u
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
usage() {
    printf '%s\n' 'Usage: sh collect.sh check [export-parent-directory]' \
        '       sh collect.sh start [ControllerRegistryID]' \
        '       sh collect.sh stop [ControllerRegistryID]' \
        'check exports to the script directory unless another existing directory is supplied.' \
        'start/stop export to the script directory; no ID applies to all controllers.' >&2
}
[ "$#" -ge 1 ] || { usage; exit 64; }
mode=$1
shift
export_parent=$script_dir
controller_id=
case "$mode" in
    check)
        [ "$#" -le 1 ] || { usage; exit 64; }
        [ "$#" -eq 0 ] || export_parent=$1
        ;;
    start|stop)
        [ "$#" -le 1 ] || { usage; exit 64; }
        [ "$#" -eq 0 ] || controller_id=$1
        ;;
    *) usage; exit 64 ;;
esac
tool=$script_dir/wakeupctl-x86_64
[ -x "$tool" ] || { printf 'Missing executable: %s\n' "$tool" >&2; exit 1; }
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/usb-xhci-wakeup.XXXXXX") || exit 1
printf 'Collecting to: %s\n' "$run_dir"
printf 'tool_version=0.2.0\ncommand=%s\ncontroller_registry_id=%s\n' "$mode" "${controller_id:-all}" > "$run_dir/run.txt"
date -u '+utc=%Y-%m-%dT%H:%M:%SZ' >> "$run_dir/run.txt"
uname -a > "$run_dir/uname.txt" 2> "$run_dir/uname-error.txt"
sw_vers > "$run_dir/sw_vers.txt" 2> "$run_dir/sw_vers-error.txt"
if [ -n "$controller_id" ]; then
    "$tool" --plist "$run_dir/status.plist" --text "$run_dir/status.txt" "$mode" "$controller_id" > "$run_dir/control-output.txt" 2> "$run_dir/control-errors.txt"
else
    "$tool" --plist "$run_dir/status.plist" --text "$run_dir/status.txt" "$mode" > "$run_dir/control-output.txt" 2> "$run_dir/control-errors.txt"
fi
control_status=$?
printf 'control_exit_status=%s\n' "$control_status" >> "$run_dir/run.txt"
cat "$run_dir/control-errors.txt" >&2
[ ! -f "$run_dir/status.txt" ] || cat "$run_dir/status.txt"
export_ok=0
if [ ! -d "$export_parent" ]; then
    printf 'Export parent is absent: %s\n' "$export_parent" > "$run_dir/export-errors.txt"
else
    export_parent=$(CDPATH= cd -- "$export_parent" && pwd)
    exported=$export_parent/$(basename "$run_dir")
    if mkdir "$exported" 2> "$run_dir/export-errors.txt"; then
        if cp -p "$run_dir"/* "$exported/" 2>> "$run_dir/export-errors.txt"; then
            export_ok=1
            for file in "$run_dir"/*; do
                [ "$(basename "$file")" = export-errors.txt ] && continue
                if ! cmp -s "$file" "$exported/$(basename "$file")"; then
                    printf 'Export mismatch: %s\n' "$(basename "$file")" >> "$run_dir/export-errors.txt"
                    export_ok=0
                fi
            done
        fi
        cp -p "$run_dir/export-errors.txt" "$exported/export-errors.txt" 2>/dev/null || export_ok=0
        if [ "$export_ok" -eq 1 ]; then
            # Persist removable-media copies before reporting success.
            sync
            printf 'Exported and byte-verified results: %s\n' "$exported"
        fi
    fi
fi
if [ "$export_ok" -ne 1 ]; then
    cat "$run_dir/export-errors.txt" >&2
    printf 'Export failed; complete original results remain at: %s\n' "$run_dir" >&2
    exit 6
fi
printf 'Original results retained at: %s\n' "$run_dir"
exit "$control_status"
