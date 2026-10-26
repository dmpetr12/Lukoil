#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
source "$project/update_panel.sh"
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/source" "$fixture/runtime"
printf 'new config' > "$fixture/source/config.json"
printf 'new lines' > "$fixture/source/lines.json"
printf 'new schedule' > "$fixture/source/schedule.json"
printf 'cabinet config' > "$fixture/runtime/config.json"
printf 'cabinet maintenance' > "$fixture/runtime/maintenance.json"
seed_runtime "$fixture/source" "$fixture/runtime"
[[ $(< "$fixture/runtime/config.json") == 'cabinet config' ]]
[[ $(< "$fixture/runtime/lines.json") == 'new lines' ]]
[[ $(< "$fixture/runtime/schedule.json") == 'new schedule' ]]
[[ $(< "$fixture/runtime/maintenance.json") == 'cabinet maintenance' ]]
printf 'cabinet lines' > "$fixture/runtime/lines.json"
seed_runtime "$fixture/source" "$fixture/runtime"
[[ $(< "$fixture/runtime/lines.json") == 'cabinet lines' ]]
mkdir -p "$fixture/runtime/releases/old" "$fixture/runtime/releases/new" "$fixture/backup"
ln -s "$fixture/runtime/releases/new" "$fixture/runtime/current"
sudo() { printf '%s\n' "$*" >> "$fixture/sudo-calls"; }
if (rollback_install "$fixture/runtime" "$fixture/runtime/releases/old" "$fixture/backup" 19); then
    echo 'Rollback unexpectedly succeeded' >&2
    exit 1
else
    [[ $? == 19 ]]
fi
[[ $(readlink "$fixture/runtime/current") == "$fixture/runtime/releases/old" ]]
[[ $(< "$fixture/sudo-calls") == 'systemctl restart lukoil-backend.service' ]]
ln -sfn "$fixture/runtime/releases/new" "$fixture/runtime/current"
set +e
(
    set -Ee
    printf -v rollback_command 'rollback_install %q %q %q "$?"' \
        "$fixture/runtime" "$fixture/runtime/releases/old" "$fixture/backup"
    trap "$rollback_command" ERR
    false
)
failure_code=$?
set -e
[[ "$failure_code" == 1 ]]
[[ $(readlink "$fixture/runtime/current") == "$fixture/runtime/releases/old" ]]
bash "$project/update_panel.sh" --check
echo 'Deployment checks passed: settings preserved; missing files seeded; rollback and error trap work.'
