#!/usr/bin/env bash
set -euo pipefail

binary=${1:-build/hideousfs-fuse}
tmp=
mount_pid=

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

cleanup() {
    if [[ -n "${tmp}" ]]; then
        if mount | grep -q "on ${tmp}/mnt "; then
            umount "${tmp}/mnt" >/dev/null 2>&1 || true
        fi
        if [[ -n "${mount_pid}" ]]; then
            wait "${mount_pid}" >/dev/null 2>&1 || true
        fi
        rm -rf "${tmp}"
    fi
}

start_mount() {
    local mode=$1

    "${binary}" "--extension=${mode}" --foreground "${tmp}/backing" "${tmp}/mnt" &
    mount_pid=$!

    for _ in {1..50}; do
        if mount | grep -q "on ${tmp}/mnt "; then
            return
        fi
        sleep 0.1
    done

    fail "mount did not become ready for extension=${mode}"
}

stop_mount() {
    if mount | grep -q "on ${tmp}/mnt "; then
        umount "${tmp}/mnt"
    fi
    if [[ -n "${mount_pid}" ]]; then
        wait "${mount_pid}" >/dev/null 2>&1 || true
        mount_pid=
    fi
}

assert_file() {
    local path=$1
    local expected=$2

    [[ -f "${path}" ]] || fail "missing file ${path}"
    [[ "$(cat "${path}")" == "${expected}" ]] ||
        fail "unexpected contents in ${path}"
}

reset_tree() {
    tmp=$(mktemp -d /private/tmp/hideousfs-fuse-test.XXXXXX)
    mkdir -p "${tmp}/backing" "${tmp}/mnt"
}

"${binary}" --selftest >/dev/null

reset_tree
trap cleanup EXIT

mkdir -p "${tmp}/backing/src"
printf 'c file\n' > "${tmp}/backing/leaf.c"
printf 'h file\n' > "${tmp}/backing/src/header.h"
printf 'plain\n' > "${tmp}/backing/Readme"

start_mount directory
assert_file "${tmp}/mnt/c/leaf" "c file"
assert_file "${tmp}/mnt/src/h/header" "h file"
printf 'created\n' > "${tmp}/mnt/h/newleaf"
assert_file "${tmp}/backing/newleaf.h" "created"
mv "${tmp}/mnt/h/newleaf" "${tmp}/mnt/c/newleaf"
[[ ! -e "${tmp}/backing/newleaf.h" ]] || fail "old h backing file remains"
assert_file "${tmp}/backing/newleaf.c" "created"
rm "${tmp}/mnt/c/newleaf"
[[ ! -e "${tmp}/backing/newleaf.c" ]] || fail "unlink left c backing file"
stop_mount
rm -rf "${tmp}"
tmp=

reset_tree
mkdir -p "${tmp}/backing/c" "${tmp}/backing/h" "${tmp}/backing/src/h"
printf 'c file\n' > "${tmp}/backing/c/leaf"
printf 'h file\n' > "${tmp}/backing/src/h/header"
printf 'plain\n' > "${tmp}/backing/Readme"
printf 'hidden real\n' > "${tmp}/backing/leaf.c"

start_mount suffix
assert_file "${tmp}/mnt/leaf.c" "c file"
assert_file "${tmp}/mnt/src/header.h" "h file"
[[ ! -e "${tmp}/mnt/c" ]] || fail "mapped extension directory is visible"
printf 'created\n' > "${tmp}/mnt/newleaf.h"
assert_file "${tmp}/backing/h/newleaf" "created"
mv "${tmp}/mnt/newleaf.h" "${tmp}/mnt/newleaf.c"
[[ ! -e "${tmp}/backing/h/newleaf" ]] || fail "old h backing file remains"
assert_file "${tmp}/backing/c/newleaf" "created"
rm "${tmp}/mnt/newleaf.c"
[[ ! -e "${tmp}/backing/c/newleaf" ]] || fail "unlink left c backing file"
stop_mount
rm -rf "${tmp}"
tmp=

reset_tree
mkdir -p "${tmp}/backing/c"
printf 'pass c file\n' > "${tmp}/backing/c/leaf"
printf 'pass suffix file\n' > "${tmp}/backing/leaf.c"

start_mount pass
assert_file "${tmp}/mnt/c/leaf" "pass c file"
assert_file "${tmp}/mnt/leaf.c" "pass suffix file"
stop_mount
rm -rf "${tmp}"
tmp=

echo "FUSE smoke tests passed"
