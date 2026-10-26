#!/usr/bin/env bash
set -euo pipefail
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
source "$project/update_panel.sh"
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture" 2>/dev/null || printf "Temporary Git fixtures retained: %s\n" "$fixture"' EXIT
# Keep fixture repositories synchronous, including on Git for Windows.
git() { command git -c maintenance.auto=false -c gc.auto=0 "$@"; }
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
git init --bare --initial-branch=master "$fixture/remote.git" > "$fixture/git-test.log" 2>&1
git clone "$fixture/remote.git" "$fixture/author" >> "$fixture/git-test.log" 2>&1
git -C "$fixture/author" config user.name 'Deployment Test'
git -C "$fixture/author" config user.email 'deployment-test@example.invalid'
printf 'first\n' > "$fixture/author/version.txt"
git -C "$fixture/author" add version.txt
git -C "$fixture/author" commit -m first >> "$fixture/git-test.log" 2>&1
git -C "$fixture/author" push -u origin master >> "$fixture/git-test.log" 2>&1
git clone "$fixture/remote.git" "$fixture/panel" >> "$fixture/git-test.log" 2>&1
printf 'second\n' > "$fixture/author/version.txt"
git -C "$fixture/author" commit -am second >> "$fixture/git-test.log" 2>&1
git -C "$fixture/author" push >> "$fixture/git-test.log" 2>&1
pull_sources "$fixture/panel" >> "$fixture/git-test.log" 2>&1
[[ $(git -C "$fixture/panel" rev-parse HEAD) == "$(git -C "$fixture/author" rev-parse HEAD)" ]]
printf 'local cabinet edit\n' > "$fixture/panel/version.txt"
if pull_sources "$fixture/panel" >> "$fixture/git-test.log" 2>&1; then
    echo 'Dirty Git checkout was not rejected' >&2
    exit 1
fi
[[ $(tr -d '\r\n' < "$fixture/panel/version.txt") == 'local cabinet edit' ]]
git -C "$fixture/panel" config user.name 'Deployment Test'
git -C "$fixture/panel" config user.email 'deployment-test@example.invalid'
git -C "$fixture/panel" commit -am local >> "$fixture/git-test.log" 2>&1
local_revision=$(git -C "$fixture/panel" rev-parse HEAD)
printf 'third\n' > "$fixture/author/version.txt"
git -C "$fixture/author" commit -am third >> "$fixture/git-test.log" 2>&1
git -C "$fixture/author" push >> "$fixture/git-test.log" 2>&1
if pull_sources "$fixture/panel" >> "$fixture/git-test.log" 2>&1; then
    echo 'Diverged Git branches were not rejected' >&2
    exit 1
fi
[[ $(git -C "$fixture/panel" rev-parse HEAD) == "$local_revision" ]]
bash "$project/update_panel.sh" --check
echo 'Deployment checks passed: settings, rollback, Git fast-forward, dirty checkout and divergent branches.'
