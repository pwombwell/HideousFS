# HideousFS FUSE Specification

This document describes the Unix/macOS FUSE product. The shared mapping rules,
history, terminology and examples are in `Spec.md`.

## Product Summary

The FUSE product mounts a normal Unix directory and presents a projected view of
that directory at a mount point. It translates between Unix-style names such as
`leaf.c` and projected names such as `c/leaf`.

It is not a RISC OS image filing system. It does not use FileSwitch, image files,
RISC OS filetypes or RISC OS catalogue operations.

## Path Syntax

The FUSE product operates on Unix paths:

- `/` is the directory separator.
- `.` is the extension separator.

With `extension=directory`, the backing directory stores Unix-friendly names and
the mounted view presents recognised extensions as directories:

```text
backing:  leaf.c
view:     c/leaf
```

With `extension=suffix`, the backing directory stores recognised extensions as
directories and the mounted view presents Unix-friendly suffixes:

```text
backing:  c/leaf
view:     leaf.c
```

The same configured reverse list is used by both mappings. The selected extension
mapping decides which direction is used for directory presentation and which
direction is used for backing-store operations.

## Command Line

A first useful command line can be:

```text
hideousfs-fuse [options] <backing-directory> <mount-point>
```

Useful options:

```text
--extension=directory|suffix|pass
--filetypes=pass|suffix|xattr
--reverse=c,h,a,cpp,c++,o,s
--config=<file>
--foreground
--readonly
--debug
```

`--extension=pass` disables leaf-name projection while still allowing RISC OS
filetype representation to be converted between xattrs and comma suffixes.

The default extension mapping may be `directory`. The default reverse list may
match the RISC OS product's default table. The default filetype representation
should be `pass`.

## Configuration

The FUSE product may either read a normal config file or accept all useful
settings from command-line options. The config syntax should match the shared
HideousFS config syntax where possible:

```text
extension directory
reverse c h a cpp c++ o s
ignore Scratch
virtualdir c h o
```

Unlike the RISC OS product, there is no image/config file selected by filetype.
If a config file lives inside the backing directory, it is an ordinary file
unless hidden by `ignore`.

## Directory Listing

`readdir` should present the projected view.

With `extension=directory`, if the backing directory contains:

```text
leaf.c
leaf.h
Readme
```

then the mounted view should contain:

```text
c
h
Readme
```

and `readdir("/c")` should contain:

```text
leaf
```

The directories `c` and `h` are synthetic extension buckets. They need not exist
in the backing directory.

With `extension=suffix`, the direction is reversed. If the backing directory
contains:

```text
c/leaf
h/leaf
Readme
```

then the mounted view should contain:

```text
leaf.c
leaf.h
Readme
```

## Lookup and File Operations

For ordinary file operations, the FUSE layer should translate the presented path
to the backing path and then delegate to the host filesystem.

Required operations for a useful read/write implementation:

```text
getattr
readdir
open
read
write
create
truncate / setattr
unlink
rename
mkdir
rmdir
release
fsync
```

A read-only prototype only needs:

```text
getattr
readdir
open
read
release
```

## Writes

With `extension=directory`:

```text
create c/leaf   -> create leaf.c
write  h/foo    -> write foo.h
unlink c/leaf   -> unlink leaf.c
rename c/foo h/foo -> rename foo.c to foo.h
```

With `extension=suffix`:

```text
create leaf.c   -> create c/leaf
write  foo.h    -> write h/foo
unlink leaf.c   -> unlink c/leaf
rename foo.c foo.h -> rename c/foo to h/foo
```

Creating a file inside a missing synthetic bucket should work if the bucket name
is in the configured reverse list. The synthetic bucket does not need to exist in
the backing directory.

## Synthetic Directories

Synthetic extension buckets are directory entries projected from matching files
or from `virtualdir` entries.

With `extension=directory`, a directory such as `c` exists if any backing file in
the same backing directory maps into it, such as `leaf.c`, or if `virtualdir c`
requests it.

Empty synthetic buckets may be volatile in v1. Persisting them can be added later
through configuration.

## Collision Policy

The FUSE product should use the same simple v1 collision policy as the shared
specification, translated into Unix syntax.

With `extension=directory`, a backing directory containing both:

```text
leaf.c
c/
```

would cause `c` to be both a synthetic extension bucket and a real directory. For
v1, the synthetic bucket wins and the real backing directory named `c` is hidden
from the projected view.

With `extension=suffix`, a backing directory containing both:

```text
c/leaf
leaf.c
```

would cause `leaf.c` to be both a projected file and a real file. For v1, the
projected file wins and the real backing file named `leaf.c` is hidden from the
projected view.

Prefer deterministic behaviour and diagnostics over clever merging.

## Symlinks

Symlink support can be deferred.

For a first read/write implementation, either reject symlink operations with a
clear error or treat symlinks as ordinary backing filesystem objects without
following them during path translation.

Do not allow symlinks to escape the backing root when resolving translated paths.

## Case Sensitivity

The FUSE product should not pretend to be case-insensitive unless a specific mode
is added for that later.

On macOS, the underlying filesystem may itself be case-insensitive. The FUSE
implementation should not depend on case sensitivity for correctness.

## RISC OS Filetype Representation

The FUSE product should optionally understand and convert between two
incompatible representations of RISC OS filetypes.

Historically, RISC OS metadata has often been represented on Unix filesystems by
appending metadata to the filename. For filetyped files this is a comma followed
by the three-digit hexadecimal filetype. For untyped files, RPCEmu and similar
HostFS implementations may instead append raw load/exec addresses as
`,load-exec`. For example:

```text
HideousFS,ffa        RISC OS module
Smoke,ffb            BASIC file
potato,0-741829fc    untyped file with load address &00000000 and exec address &741829FC
```

Text files are filetype `&FFF`, including normal C and header source files, and
should not normally gain a comma suffix merely because they are text.

Some emulators and host filesystems may instead store RISC OS metadata in the
extended attribute, `user.RISC_OS.LoadExec`.

This xattr uses the IXFS/RISC OS on Linux layout: native host-endian `uint32_t`
words for load, exec and optionally attributes. For timestamped RISC OS files,
HideousFS may store the filetype marker and filetype bits in the xattr as
`&FFFttt00 / &00000000`, with the timestamp itself represented by the host file’s
mtime.

Filetype/load-exec conversion should be independent of leaf-name projection. It
should be possible to enable filetype conversion even when the `leaf.c` ↔
`c/leaf` mapping is disabled.

Supported filetype conversion modes:

```text
--filetypes=pass    leave comma suffixes and xattrs as they are stored
--filetypes=suffix  present RISC OS metadata as comma suffixes
--filetypes=xattr   present RISC OS metadata as user.RISC_OS.LoadExec xattrs
```

`pass` mode does not reinterpret or convert RISC OS metadata.

`suffix` mode exposes RISC OS metadata using comma suffixes. If the backing file
has `user.RISC_OS.LoadExec`, the mounted view should present an appropriate
`,ttt` or `,load-exec` suffix. Filetype `&FFF` should not gain a `,fff` suffix by
default.

`xattr` mode exposes RISC OS metadata using `user.RISC_OS.LoadExec`. If the
backing file has a comma suffix, the mounted view should present the base
filename and synthesise the xattr.

If xattrs and comma suffixes are both present, xattrs are authoritative. A comma
suffix that disagrees with the xattr should be treated as ordinary filename text,
not as metadata.

For filetype `&FFF`, no comma suffix should be added by default.

### Interaction with Extension Mapping

Comma metadata suffixes are outside the leaf-name projection. The extension
mapping operates on the logical base filename, and the metadata suffix is then
preserved or generated on the projected leaf.

For example, with `extension=suffix`:

```text
backing:  c/script,feb
view:     script.c,feb
```

The `c/script` part maps to `script.c`; the `,feb` part remains the RISC OS
filetype suffix for the projected file.

Similarly, with `extension=directory`:

```text
backing:  script.c,feb
view:     c/script,feb
```

For raw load/exec suffixes, the same rule applies:

```text
backing:  o/potato,0-741829fc
view:     potato.o,0-741829fc
```

This rule deliberately allows non-text files to live inside source-extension
buckets. Such files may be unusual, but they should still project consistently.
An Obey file stored as `c/script,feb` should therefore appear as
`script.c,feb`, not as an unprojected special case.


## Suggested Source Layout

```text
src/
  Mapping.c     shared mapping logic
  Mapping.h
  Config.c      shared config parsing
  Config.h
  ImageFS.c     RISC OS FileSwitch/image FS glue
  Fuse.c        FUSE glue

Makefile
```

The shared mapping code should not include FUSE headers and should not include
RISC OS headers.

The extension separator and directory separator could be a #defined
constant, if that makes it easier to share the source. The same binary
will never run as FUSE or ImageFS.


## Implementation Milestones

### 1. Read-only FUSE Prototype

- Mount a backing directory at a mount point.
- Implement `getattr`, `readdir`, `open`, `read` and `release`.
- Hard-code `extension=directory` and the default reverse list.
- Present `leaf.c` as `c/leaf`.

### 2. Shared Mapping Library

- Move pure mapping code into `HideousMap.c` / `HideousMap.h`.
- Use the same mapping code from the RISC OS product and the FUSE product.
- Add unit tests for mapping in both directions.

### 3. Writes

- Implement `create`, `write`, `truncate`, `unlink` and `rename`.
- Ensure writes through synthetic buckets create the correct backing file.
- Add deterministic tests that inspect the backing directory after each write.

### 4. Configuration

- Parse `extension`, `filetypes`, `reverse`, `ignore` and `virtualdir`.
- Accept config from a file and/or command-line options.
- Add tests for custom reverse lists and ignored names.

### 5. Collision Diagnostics

- Detect simple collisions during `readdir`.
- Apply the v1 collision policy deterministically.
- Add optional debug output listing hidden backing entries.

### 6. Optional Filetype Representation Conversion

- Implement `--filetypes=pass` as the default.
- Implement `--filetypes=suffix`.
- Implement `--filetypes=xattr`.
- Allow filetype conversion to run with `--extension=pass`.
- Support both `,ttt` filetype suffixes and `,load-exec` raw load/exec suffixes.
- Prefer xattrs over suffixes when both are present.
- Treat disagreeing comma suffixes as ordinary filename text.
- Do not add `,fff` suffixes for text files by default.

## Testing

The FUSE product should be tested with normal Unix tools and scripts, not with
RISC OS BASIC.

Tests should create temporary backing and mount directories, mount the FUSE
filesystem, perform operations through the mounted view, and inspect both the
mounted view and the backing directory.

A first test harness can be a shell script or Python script. Python is likely
more convenient because it can create directory trees, run subprocesses, compare
expected results, and inspect xattrs.

Useful tests:

- `extension=directory` directory listings: `leaf.c` and `leaf.h` appear as
  `c/leaf` and `h/leaf`.
- `extension=suffix` directory listings: `c/leaf` and `h/leaf` appear as
  `leaf.c` and `leaf.h`.
- `extension=pass` leaves names unchanged.
- Opening and reading through the projected path reads the backing file.
- Creating through a projected path creates the expected backing file.
- Renaming between projected paths renames the expected backing file.
- Moving a file whose backing name should not change from outside the projected
  view into the projected view is effectively a metadata/directory operation,
  not a destructive rewrite.
- Such a move must preserve file contents, size, timestamps and xattrs, and must
  not truncate the file, overwrite it with zero bytes, or otherwise corrupt it.
- Deleting through a projected path deletes the expected backing file.
- Creating a file inside a missing synthetic extension directory works.
- Collision handling is deterministic and matches the documented policy.
- `filetypes=pass` leaves comma suffixes and xattrs unchanged.
- `filetypes=suffix` presents xattrs as comma suffixes, including raw
  `,load-exec` suffixes.
- `filetypes=xattr` presents comma suffixes as `user.RISC_OS.LoadExec` xattrs.
- Filetype `&FFF` does not gain a `,fff` suffix by default.
- A disagreeing comma suffix is treated as ordinary filename text when an xattr
  is authoritative.
- `filetypes=suffix` and `filetypes=xattr` also work with `extension=pass`.

Each test should compare expected and actual directory trees in a stable textual
form. For write and rename tests, inspect the backing directory after unmounting
as well as through the mounted view while mounted. Tests that move an existing
file into the projected view should compare a checksum before and after the move,
and should also check that the backing file was renamed or left in place exactly
as expected.

The test harness should always unmount the FUSE filesystem in cleanup, even if a
test fails.
