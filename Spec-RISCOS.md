# HideousFS RISC OS Specification

This document describes the RISC OS image filing system product. The shared
mapping rules, history, terminology and examples are in `Spec.md`.

## Product Summary

The RISC OS product is an image filing system module written in C. A file of the
HideousFS image filetype inside a RISC OS directory acts as a portal/config file.
Opening that image through FileSwitch presents a projected view of the containing
HostFS directory.

The filename `View` is only a convention used in examples. The image/config file
is identified by filetype, not by name.

Until an official filetype is allocated, use filetype `&001` from the user area.
This is temporary and may clash with existing uses.

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
parent directory of the image/config file.

## RISC OS Path Syntax

The RISC OS product operates on RISC OS paths:

- `.` is the directory separator.
- `/` is the extension separator used by RISC OS tools and compilers.

With `extension directory`:

```text
backing:  leaf/c
view:     c.leaf
```

With `extension suffix`:

```text
backing:  c.leaf
view:     leaf/c
```

## Image File Behaviour

The active image/config file itself is excluded from the projected catalogue by
default, regardless of its leafname.

Other image/config files in the same backing directory are ordinary catalogue
entries unless hidden by configuration.

The image/config file contains configuration, not the file data. The real files
remain in the containing HostFS directory.

## Build Requirements

It should be compiled on macOS using `ncc-riscos`. Other tools are `cmunge`,
which is a replacement for Acorn's `cmhg` and processes files with that
extension. `drlink` should be used to link the objects, with `stubs.a`.

It should all consistently compile and link with 32-bit APCS, and perhaps
generate a 26-bit module too.

It should be future-proof enough to allow compilation for 64-bit RISC OS using
its toolchain later, but that can be a future step.

## RISC OS Milestones

### 1. Minimal Image FS Skeleton

- Build a C module.
- Register/recognise the development image filetype `&001`.
- Do not rely on the leafname `View`, except as a convenient convention in
  examples.
- Opening the image should show a hard-coded catalogue entry such as `Hello`.
- No backing directory logic yet.

### 2. Mirror Backing Directory Read-Only

- When `HostFS:$.foo.View` is opened, derive backing directory `HostFS:$.foo`.
- Catalogue the contents of the backing directory.
- Exclude the image/config file itself, regardless of its leafname.
- Initially show names unchanged.
- Support opening and reading files through FileSwitch from the backing
  directory.

### 3. Add Name Transformation

- With `extension directory`, apply `leaf/c -> c.leaf` during catalogue
  enumeration.
- With `extension directory`, when opening `c.leaf`, map back to `leaf/c`.
- Keep the mapping code symmetric so `extension suffix` can apply the reverse
  mapping later.
- Unknown names pass through unchanged.

### 4. Add Writes

- Saving `c.leaf` should create/update backing file `leaf/c`.
- Saving `h.foo` should create/update backing file `foo/h`.
- Saving `Readme` should create/update backing file `Readme`.
- Creating a file inside a missing synthetic bucket should work.

## Testing

Add RISC OS BASIC tests that create deterministic backing trees, catalogue the
image view, and check that reads, renames, deletes and saves map to the expected
backing files.

A RISC OS `BASIC` program should create at least two directory structures
representing both mappings:

- an `extension directory` backing directory containing names such as `leaf/c`
- an `extension suffix` backing directory containing names such as `c.leaf`

The test program should create an image config file in each directory, set the
development filetype `&001`, and catalogue the directory through the image view.

Output should be a small deterministic tree, similar to the Unix `tree` command,
so it can be diffed against expected output. The tree output helper could live in
a `LIBRARY`.

A second `BASIC` program, or perhaps a `LIBRARY` included by the first, should
exist to modify such a directory structure to ensure file renaming matches
expectations. This should include renaming and deleting both files and
directories.

A third test should save files to ensure the correct backing file is modified. A
simple sequence number could be written, and then checked outside the image FS to
ensure the correct file was modified.