# HideousFS Specification

Write a RISC OS image filing system module in C, not C++. Use DOSFS only as
reference material for how an image filing system talks to FileSwitch and how
image handlers are structured. Do not fork DOSFS wholesale unless a small
useful piece is clearly reusable.

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

## Goal

A file of the HideousFS image filetype inside a HostFS directory acts as the
image file / portal. The filename `View` is only a convention used in
examples; any leafname with the correct filetype should work. Opening that
image through RISC OS should show a transformed view of the containing
directory. The image file itself contains configuration, not the file data.

Until an official filetype is allocated, use filetype `&001` from the user area.
This intentionally risks clashing with existing uses such as music files and
should be treated as temporary.

Use filing system number `666` for development.

Example backing directory:

```text
HostFS:$.myproject
  View     (filetype &001; name is arbitrary)
  leaf1/c
  leaf2/c
  leaf4/h
  Readme
```

Opening:

```text
HostFS:$.myproject.View
```

should show something like:

```text
c.leaf1
c.leaf2
h.leaf4
Readme
```

The point is to allow Unix/GitHub/editor-friendly names such as `leaf/c`, which
would be `leaf.c` in the Unix view of the filesystem, while RISC OS sees
traditional RISC OS-ish names such as `c.leaf`.

## Implementation Language

Use C.

Avoid C++.

Assume normal RISC OS module style.

## Reference Material

Use the DOSFS source and PRM documentation as references for:

- registering an image filing system
- image handler entry points
- how FileSwitch opens an image file and redirects operations into the image FS
- catalogue enumeration
- file open/read/write calls
- path translation behaviour

HideousFS is not a FAT/DOS parser. It is a view/proxy filesystem backed by the
parent directory of the image file.

## Core Concept

The image file, conventionally named `View` but selected by filetype `&001`
during development:

```text
HostFS:$.myproject.View
```

backs onto its containing directory:

```text
HostFS:$.myproject
```

The image/config file itself is excluded from the synthetic catalogue by
default, regardless of its leafname.

The real files remain in the backing directory. HideousFS maps apparent RISC OS
paths to backing HostFS paths.

## Operating Modes

HideousFS should support two mapping modes, selected by the image config file.

### Hideous Mode

Hideous mode is the main mode for Unix/GitHub-friendly storage.

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

### Beautiful Mode

Beautiful mode is the reverse mode.

Backing directory contains RISC OS-style names:

```text
c.leaf
h.leaf
Readme
```

The HideousFS view presents Unix-style names as seen through HostFS:

```text
leaf/c
leaf/h
Readme
```

This does not make the backing directory more useful from Unix, but it is useful
for conversion and migration. Copying files out of a Beautiful-mode image view
can produce the Unix-friendly layout. It also lets users try the mapping in
reverse without immediately changing the original directory structure.

V1 may hard-code Hideous mode. Beautiful mode should be kept in mind when
designing the path-mapping functions so that the transform is explicitly
reversible.

## Initial Name Mapping

Configured extension mappings should describe a reversible pair. In Hideous
mode, Unix-style HostFS names are shown as RISC OS-style source names. In
Beautiful mode, the same mapping is used in reverse.

Examples:

| Host name | HideousFS name |
| --- | --- |
| `leaf/c` | `c.leaf` |
| `leaf/h` | `h.leaf` |
| `leaf/s` | `s.leaf` |
| `leaf/o` | `o.leaf` |
| `leaf/a` | `a.leaf` |
| `leaf/cpp` | `cpp.leaf` |
| `leaf/c++` | `c++.leaf` |
| `Readme/md` | `Readme/md` |

The mapping must be reversible.

The active image/config file itself is not part of this mapping. It is always
excluded from the projected catalogue. Files that don't map, such as
`Readme/md` should appear unmodified.

Other image/config files in the same backing directory are ordinary catalogue
entries unless hidden by configuration. This allows, for example, two separate
views of the same directory with different mappings. A later config option such
as `ignoretype 001 fff` could hide entries by filetype, but v1 does not need it.

A simple default mapping table is acceptable initially:

| Host name | HideousFS name |
| --- | --- |
| `leaf/c` | `c.leaf` |
| `leaf/h` | `h.leaf` |
| `leaf/s` | `s.leaf` |
| `leaf/o` | `o.leaf` |
| `leaf/a` | `a.leaf` |
| `leaf/cpp` | `cpp.leaf` |
| `leaf/c++` | `c++.leaf` |

The image file, conventionally named `View` but with an arbitrary leafname,
should later be parsed as config, including the mode. Multiple `reverse`,
`ignore` and `virtualdir` entries should be concatenated to the internal list.

Example Hideous-mode config:

```text
# HideousFS
# filetype &001 during development; leafname is arbitrary
mode hideous

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
mode beautiful

reverse c h a cpp c++ o s
```

V1 can hard-code the default table before config parsing exists.

## Milestones

### 1. Minimal Image FS Skeleton

- Build a C module.
- Register HideousFS as image filing system number `666` for development.
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

- In Hideous mode, apply `leaf/c -> c.leaf`, etc. during catalogue enumeration.
- In Hideous mode, when opening `c.leaf`, map back to `leaf/c`.
- Keep the mapping code symmetric so Beautiful mode can apply `c.leaf -> leaf/c`
  during catalogue enumeration and map `leaf/c` back to `c.leaf` for file
  operations.
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

- Hideous mode hides real backing directories whose names are in the `reverse`
  list. For example, a real backing directory named `c` disappears from view,
  leaving only the synthetic `c` bucket.
- Beautiful mode hides real backing files whose extension names are in the
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
  config-selected mode deciding which direction is used for catalogue
  presentation and which direction is used for backing-store operations.

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
extension. `drlink` should be used to link the objects, with `stubs.a`.

See DOSFS, up one directory, as an example, though that compiles under RISC OS.
See PDModules, also up one directory, as an example of modules that compile
under macOS using the named tools, such as `PDModules/PDriverPDF`, compiled with
`Makefile.macos`.

## Testing

A RISC OS `BASIC` program should exist to create at least two directory
structures representing both modes:

- a Hideous-mode backing directory containing names such as `leaf/c`
- a Beautiful-mode backing directory containing names such as `c.leaf`

The test program should create an image config file in each directory, set the
development filetype `&001`, and catalogue the directory through the image view.

Output should be a small deterministic tree, similar to the Unix `tree`
command, so it can be diffed against expected output. The tree output helper
could live in a `LIBRARY`.

A second `BASIC` program, or perhaps a `LIBRARY` included by the first, should
exist to modify such a directory structure to ensure file renaming matches
expectations. This should include renaming and deleting both files and
directories.

A third test should save files to ensure the correct backing file is modified. A
simple sequence number could be written, and then checked outside the image FS
to ensure the correct file was modified.

## Repository Documents

Keep this file as the developer/implementation specification. Add a separate
`README.md` for user-facing documentation: what HideousFS does, how to create a
config image file, how Hideous and Beautiful modes behave, and the current
temporary nature of filetype `&001` and filing system number `666`.
