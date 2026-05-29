# HideousFS Specification

HideousFS is a family of projection filesystems for translating between
RISC OS-style source layouts and Unix-style source layouts. It should support
both a RISC OS image filing system product and a Unix/macOS FUSE product, sharing
as much path-mapping code and specification language as practical.

This document describes the shared behaviour and terminology. Product-specific
requirements live in `Spec-RISCOS.md` and `Spec-Fuse.md`.

## History

RISC OS historically had a 10-character filename limit, and has no file
extensions - it uses filetypes for most things. The convention for programming
is different: source files are all Text, and you have a directory named after
your file extension, such as `c`, `h`, `o`, `s`, or `Hdr`. Files inside have no
extension.

RISC OS uses `.` as a directory separator and `/` for extensions, although it
does not generally have extensions because it uses filetypes. The compiler
knows how to munge the filenames. This conflicts with other OSes, where
filenames are not split across directories.

Git repositories for RISC OS are usually in the RISC OS style, even if cross-compiling (eg. https://github.com/gerph/riscos-wimp-templates) but some are in unix style (eg. https://github.com/pwombwell/PDrivers/tree/main/PDriverPDF). This causes problems with tools that are generally designed for one and not the other.

## Goal

HideousFS has two related products:

- A RISC OS image filing system module. This presents a transformed RISC OS view
  of a backing HostFS directory through an image/config file.
- A Unix/macOS FUSE filesystem. This mounts a backing Unix directory and presents
  a transformed Unix view.

Both products use the same mapping concepts and should share the pure mapping
code. They differ in path syntax, integration points, and platform metadata.

The purpose is to let each system use its native tools naturally. A RISC OS C
compiler presented with `#include "header.h"` conventionally looks for
`h.header`; a Unix compiler presented with the same source looks for `header.h`.
There have been RISC OS compilers hosted on Unix and Unix compilers hosted on
RISC OS that understand these conventions, but HideousFS should make the common
case simpler: on Unix, use Unix tools and Unix filenames; on RISC OS, use RISC OS
tools and RISC OS filenames.

For the RISC OS product, a file of the HideousFS image filetype inside a RISC OS
directory acts as the image file / portal. The filename `View` is only a
convention used in examples; any leafname with the correct filetype should work.
Opening that image through RISC OS should show a transformed view of the
containing directory. The image file itself contains configuration, not the file
data.

For the FUSE product, a normal mount command should mount a backing Unix
directory at a chosen mount point and present the transformed view there. There
is no image file or FileSwitch integration in the FUSE product.

Until an official filetype is allocated, use filetype `&001` from the user area.
This intentionally risks clashing with existing uses such as soundtracker files and
should be treated as temporary.

Example backing directory (viewed from RISC OS):

```text
HostFS::HostFS.$.myproject
  View     (filetype &001; name is arbitrary)
  leaf1/c
  leaf2/c
  leaf4/h
  Readme
```

Opening:

```text
HostFS::HostFS.$.myproject.View
```

should show something like:

```text
c
h
Readme
```

where `c` is a directory that contains `leaf1` and `leaf2`, and `h` is a directory that contains `leaf4`.

The point is to allow Unix/GitHub/editor-friendly names such as `leaf/c`, which
would be `leaf.c` in the Unix view of the filesystem, while RISC OS sees
traditional RISC OS-ish names such as `c.leaf`.

## Implementation Language

Use C for the shared mapping code and for both products initially.

Avoid C++.

The shared mapping code should avoid RISC OS-specific and FUSE-specific APIs so
that it can be reused by both products. Product glue should live in separate
source files.

## Reference Material

For the RISC OS product, use the DOSFS source and PRM documentation as references
for FileSwitch and image filing system integration. See `Spec-RISCOS.md` for the
RISC OS-specific details.

For the FUSE product, use libfuse on Linux and macFUSE on macOS as references for
filesystem operation entry points. See `Spec-Fuse.md` for the FUSE-specific
details.

HideousFS is not a FAT/DOS parser and is not a physical filesystem. It is a
view/proxy filesystem backed by an ordinary directory.

## Core Concept

The image file, conventionally named `View` but selected by filetype `&001`
during development:

```text
HostFS::HostFS.$.myproject.View
```

backs onto its containing directory:

```text
HostFS::HostFS.$.myproject
```

The image/config file itself is excluded from the synthetic catalogue by
default, regardless of its leafname.

The real files remain in the backing directory. HideousFS maps apparent RISC OS
paths to backing HostFS paths.

For the FUSE product, there is no image/config file in the mounted view unless a
normal file with that name exists in the backing directory. The backing directory
is supplied explicitly as a mount option or command-line argument, and the mount
point exposes the projected view.

## Extension Mapping

HideousFS should support configurable extension mapping. Extension mapping controls
where source-style extensions live: as directory components, or attached to the
leaf name. It is separate from RISC OS filetype representation.

### `extension directory`

`extension directory` presents recognised extensions as directory components.
This is the main mapping for Unix/GitHub-friendly backing storage.

Backing directory contains Unix-style names as seen by RISC OS HostFS, for
example:

```text
leaf/c
leaf/h
Readme
```

The HideousFS view presents RISC OS-style source layout:

```text
c.leaf
h.leaf
Readme
```

This is useful when the same files are also viewed from Unix, where HostFS
presents `leaf/c` as `leaf.c`.

### `extension suffix`

`extension suffix` is the reverse mapping: recognised extension directories are
presented as extensions attached to the leaf name.

Backing directory contains RISC OS-style names, where `c` contains `leaf` and `h` contains `leaf`:

```text
c
h
Readme
```

The HideousFS view presents Unix-style names as seen through HostFS:

```text
leaf/c
leaf/h
Readme
```

This does not make the backing directory more useful from Unix, but it is useful
for conversion and migration. Copying files out of an `extension suffix` image view
can produce the Unix-friendly layout. It also lets users try the mapping in
reverse without immediately changing the original directory structure.

V1 may hard-code `extension directory`. `extension suffix` should be kept in mind
when designing the path-mapping functions so that the transform is explicitly
reversible.

## Initial Extension Mapping

Configured extension mappings should describe a reversible pair. With
`extension directory`, extension suffixes in the backing store are shown as
extension directories in the projected view. With `extension suffix`, the same
mapping is used in reverse.

The RISC OS product transforms RISC OS-style names using `.` as the directory
separator and `/` as the extension separator. The FUSE product transforms
Unix-style names using `/` as the directory separator and `.` as the extension
separator.

Examples:

| RISC OS suffix form | RISC OS directory form | Unix suffix form | Unix directory form |
| --- | --- | --- | --- |
| `leaf/c` | `c.leaf` | `leaf.c` | `c/leaf` |
| `leaf/h` | `h.leaf` | `leaf.h` | `h/leaf` |
| `leaf/s` | `s.leaf` | `leaf.s` | `s/leaf` |
| `leaf/o` | `o.leaf` | `leaf.o` | `o/leaf` |
| `leaf/a` | `a.leaf` | `leaf.a` | `a/leaf` |
| `leaf/cpp` | `cpp.leaf` | `leaf.cpp` | `cpp/leaf` |
| `leaf/c++` | `c++.leaf` | `leaf.c++` | `c++/leaf` |
| `Readme/md` | `Readme/md` | `Readme.md` | `Readme.md` |

The mapping must be reversible.

The active image/config file itself is not part of this mapping. It is always
excluded from the projected catalogue. Files that don't map, such as
`Readme/md` should appear unmodified.

Other image/config files in the same backing directory are ordinary catalogue
entries unless hidden by configuration. This allows, for example, two separate
views of the same directory with different mappings. A later config option such
as `ignoretype 001 fff` could hide entries by filetype, but v1 does not need it.

 
A simple default mapping table is acceptable initially. In the RISC OS product,
`extension directory` maps from the left column to the right column during
catalogue enumeration, and maps back during file operations:

| RISC OS suffix form | RISC OS directory form |
| --- | --- |
| `leaf/c` | `c.leaf` |
| `leaf/h` | `h.leaf` |
| `leaf/s` | `s.leaf` |
| `leaf/o` | `o.leaf` |
| `leaf/a` | `a.leaf` |
| `leaf/cpp` | `cpp.leaf` |
| `leaf/c++` | `c++.leaf` |

The image file, conventionally named `View` but with an arbitrary leafname,
should later be parsed as config, including the extension mapping. Multiple `reverse`,
`ignore` and `virtualdir` entries should be concatenated to the internal list.

Example `extension directory` config:

```text
# HideousFS
# filetype &001 during development; leafname is arbitrary
extension directory

# Map all of c, h, a, cpp, c++, o, and s.
reverse c h a cpp c++
reverse o
reverse s

# Hide a non-image entry from the projected catalogue.
ignore Scratch

# Ensure several virtual directories exist.
virtualdir c h o
```

For reverse conversion, the config may instead say:

```text
# HideousFS
# filetype &001 during development; leafname is arbitrary
extension suffix

reverse c h a cpp c++ o s
```

V1 can hard-code the default table before config parsing exists.

## Milestones

### 1. Minimal Image FS Skeleton

- Build a C module.
- Register/recognise the development image filetype `&001`.
- Do not rely on the leafname `View`, except as a convenient convention in
  examples.
- Opening the image should show a hard-coded catalogue entry such as `Hello`.
- No backing directory logic yet.

### 2. Mirror Backing Directory Read-Only

- When `HostFS:$.foo.View` is opened, derive backing directory
  `HostFS:$.foo`.
- Catalogue the contents of the backing directory.
- Exclude the image/config file itself, regardless of its leafname.
- Initially show names unchanged.
- Support opening and reading files through FileSwitch from the backing
  directory.
- Read-only is sufficient at this stage.

### 3. Add Name Transformation

- With `extension directory`, apply `leaf/c -> c.leaf`, etc. during catalogue
  enumeration.
- With `extension directory`, when opening `c.leaf`, map back to `leaf/c`.
- Keep the mapping code symmetric so `extension suffix` can apply
  `c.leaf -> leaf/c` during catalogue enumeration and map `leaf/c` back to
  `c.leaf` for file operations.
- Unknown names pass through unchanged.
- Keep collision handling simple initially: either reject ambiguous names or
  log/report an error.

### 4. Add Writes

- Saving `c.leaf` should create/update backing file `leaf/c`.
- Saving `h.foo` should create/update backing file `foo/h`.
- Saving `Readme` should create/update backing file `Readme`.
- Creating a file inside a missing synthetic bucket should work; the bucket does
  not need to have existed before.

### 5. Directories

- Real directories in the backing store are real directories and should be shown
  normally.
- The same behaviour should be applied to subdirectories, with the exception of
  empty synthetic directories which need to be specified per host-directory.
- Creating a normal directory creates a real backing directory.
- Synthetic extension buckets may be useful later, but should not block v1.

## Empty Synthetic Directories

If the user creates an empty synthetic extension bucket, such as `c`, it is
acceptable for v1 to keep that state only in memory, though the user could
create it persistently in the image file with `virtualdir c`, or (relative
to the image file) as `virtualdir src.c` to create the `c` bucket below `src`.

Suggested rule:

- Non-empty synthetic buckets appear because matching backing files exist.
- Empty synthetic buckets can be created, but are volatile.
- The volatile marker is lost on reboot/module restart.
- Saving into a missing synthetic bucket still works, because the path can be
  decoded from the configured mapping.

Example:

```text
cdir c  (ie. RISC OS's mkdir)
```

records in module memory:

```text
backing_dir + bucket "c" is explicitly visible
```

Then:

```text
save c.leaf
```

or equivalent path syntax creates:

```text
leaf/c
```

Once at least one `*.c` file exists, the `c` bucket is naturally visible and the
volatile marker no longer matters.

Do not persist arbitrary empty synthetic directory state in the image config
file for v1. It can be added later if needed.

## Collision Handling

Need a defined policy for cases like:

```text
leaf/c
c.leaf
```

Both could appear as `c.leaf`.

For v1, use a simple policy:

- `extension directory` hides real backing directories whose names are in the
  `reverse` list. For example, a real backing directory named `c` disappears
  from view, leaving only the synthetic `c` bucket.
- `extension suffix` hides real backing files whose extension names are in the
  `reverse` list. For example, a real backing file named `leaf/c` disappears
  from view, leaving only the projected `c.leaf` entry.

Prefer a simple, visible diagnostic rather than cleverness.

## Design Constraint

HideousFS is a projection filesystem:

- Backing storage is the containing HostFS directory.
- The image file is a portal/config file.
- The image file is identified by filetype, not by the leafname `View` which is
  used purely as an example. Multiple image files may exist in the same
  directory with different settings.
- File operations are proxied to real files through FileSwitch.
- The same mapping table should work in both directions, with the
  config-selected extension mapping deciding which direction is used for
  catalogue presentation and which direction is used for backing-store
  operations.

Read-only first. Add writes after catalogue/open/read are reliable.

## Recommended First Target

Produce a C module that lets this work:

```text
HostFS:$.myproject.View
```

and opening it shows a read-only catalogue containing:

```text
Hello
```

Then replace `Hello` with the real backing directory enumeration.

## Compiler

It should be compiled on macOS using `ncc-riscos`. Other tools are `cmunge`,
which is a replacement for Acorn's `cmhg` and processes files with that
extension. `drlink` should be used to link the objects, with `stubs.a`. It should
all consistently compile and link with 32-bit APCS, and perhaps generate a 26-bit
module, too. It should be future-proof to allow compilation for 64-bit RISC OS
using its toolchain (gcc), but that can be a future step as I don't have the
toolchain installed (and it may even require hideous mode for all I know). An
example of compiling for 26-bit can be seen in `../GitHub/aof-toolchain/norcroft`.

See DOSFS, up one directory, as an example, though that compiles under RISC OS.
See PDModules, also up one directory, as an example of modules that compile
under macOS using the named tools, such as `PDModules/PDriverPDF`, compiled with
`Makefile.macos`.

## Testing

Both products need deterministic tests for extension mapping, filetype
representation, collisions, reads, writes, renames and deletes.

Product-specific test plans live in `Spec-RISCOS.md` and `Spec-Fuse.md`.

## Repository Documents

Keep this file as the developer/implementation specification. Add a separate
`README.md` for user-facing documentation: what HideousFS does, how to create a
config image file, how the extension mappings behave, and the current
temporary nature of filetype `&001`.
