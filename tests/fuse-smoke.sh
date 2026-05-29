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
    shift

    "${binary}" "--extension=${mode}" "$@" --foreground "${tmp}/backing" "${tmp}/mnt" &
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
    local i

    for i in {1..20}; do
        [[ -f "${path}" ]] && break
        sleep 0.1
    done
    [[ -f "${path}" ]] || fail "missing file ${path}"
    [[ "$(cat "${path}")" == "${expected}" ]] ||
        fail "unexpected contents in ${path}"
}

assert_xattr_hex() {
    local path=$1
    local expected=$2
    local actual

    actual=$(xattr -p -x user.RISC_OS.LoadExec "${path}" | tr -d ' \n' | tr '[:upper:]' '[:lower:]')
    [[ "${actual}" == "${expected}" ]] ||
        fail "unexpected LoadExec xattr on ${path}: ${actual}"
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
printf 'typed c file\n' > "${tmp}/backing/typed.c,ffb"
printf 'h file\n' > "${tmp}/backing/src/header.h"
printf 'plain\n' > "${tmp}/backing/Readme"

start_mount directory
assert_file "${tmp}/mnt/c/leaf" "c file"
assert_file "${tmp}/mnt/c/typed,ffb" "typed c file"
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
printf 'ps file\n' > "${tmp}/backing/Printout"
xattr -w -x user.RISC_OS.LoadExec "00 F5 FF FF 00 00 00 00 03 00 00 00" "${tmp}/backing/Printout"
printf 'basic file\n' > "${tmp}/backing/Smoke,ffb"
printf 'typed c file\n' > "${tmp}/backing/typed.c,ffb"

start_mount directory --filetypes=suffix
assert_file "${tmp}/mnt/Printout,ff5" "ps file"
assert_file "${tmp}/mnt/Smoke,ffb" "basic file"
assert_file "${tmp}/mnt/c/typed,ffb" "typed c file"
stop_mount
rm -rf "${tmp}"
tmp=

reset_tree
printf 'basic file\n' > "${tmp}/backing/Smoke,ffb"
printf 'typed c file\n' > "${tmp}/backing/typed.c,ffb"

start_mount directory --filetypes=xattr
assert_file "${tmp}/mnt/Smoke" "basic file"
[[ ! -e "${tmp}/mnt/Smoke,ffb" ]] || fail "comma metadata name is visible in xattr mode"
assert_xattr_hex "${tmp}/mnt/Smoke" "00fbffff0000000000000000"
assert_file "${tmp}/mnt/c/typed" "typed c file"
[[ ! -e "${tmp}/mnt/c/typed,ffb" ]] || fail "comma extension metadata name is visible in xattr mode"
assert_xattr_hex "${tmp}/mnt/c/typed" "00fbffff0000000000000000"
stop_mount
rm -rf "${tmp}"
tmp=

reset_tree
printf 'c file\n' > "${tmp}/backing/leaf.c"
printf 'basic file\n' > "${tmp}/backing/Smoke,ffb"
printf 'hidden\n' > "${tmp}/backing/Hidden"
cat > "${tmp}/hideousfs.conf" <<EOF
extension directory
filetypes xattr
reverse c h
ignore Hidden
virtualdir h
EOF

start_mount pass --config="${tmp}/hideousfs.conf"
assert_file "${tmp}/mnt/c/leaf" "c file"
assert_file "${tmp}/mnt/Smoke" "basic file"
assert_xattr_hex "${tmp}/mnt/Smoke" "00fbffff0000000000000000"
[[ ! -e "${tmp}/mnt/Hidden" ]] || fail "ignored file is visible"
! ls -A "${tmp}/mnt" | grep -qx "Hidden" || fail "ignored file is listed"
ls -A "${tmp}/mnt" | grep -qx "h" || fail "configured virtual dir is not listed"
printf 'created via virtual dir\n' > "${tmp}/mnt/h/header"
assert_file "${tmp}/backing/header.h" "created via virtual dir"
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

reset_tree
printf 'outside\n' > "${tmp}/outside"
ln -s "${tmp}/outside" "${tmp}/backing/escape"

start_mount pass
if cat "${tmp}/mnt/escape" >/dev/null 2>&1; then
    fail "backing symlink escaped the mount"
fi
stop_mount
rm -rf "${tmp}"
tmp=

echo "FUSE smoke tests passed"
