Project: HideousFS

Write a RISC OS image filing system module in C, not C++. Use DOSFS only as reference for how an image filing system talks to FileSwitch and how image handlers are structured. Do not fork DOSFS wholesale unless a small useful piece is clearly reusable.

History

RISC OS historically had a 10-char filename limit, and has no file extensions - it uses filetypes for most things. The convention for programming is different - source files are all Text, and you have a directory named after your file extension ('c', 'h', 'o', 's', 'Hdr' etc.) Files inside have no extension.

Remember RISC OS uses '.' as a directory separator and '/' for extensions (not that it has any as it uses filetypes, apart from C). The compiler knows to munge the filenames. This conflicts with all other OSes where filenames are not split across directories.


Goal

A file of the HideousFS image filetype inside a HostFS directory acts as the image file / portal. The filename `Hideous` is only a convention used in examples; any leafname with the correct filetype should work. Opening that image through RISC OS should show a transformed view of the containing directory. The image file itself contains configuration, not the file data.

Until an official filetype is allocated, use filetype `&001` from the user area. This intentionally risks clashing with existing uses such as music files and should be treated as temporary.

Use filing system number 666 for development.

Example backing directory:

    HostFS:$.myproject
      Hideous     (filetype &001; name is arbitrary)
      leaf1/c
      leaf2/c
      leaf4/h
      Readme

Opening:

    HostFS:$.myproject.Hideous

should show something like:

    c.leaf1
    c.leaf2
    h.leaf4
    Readme

The point is to allow Unix/GitHub/editor-friendly names such as `leaf/c` (which would be `leaf.c` in the Unix view of the filesystem), while RISC OS sees traditional RISC OS-ish names such as `c.leaf`.

Implementation language

Use C.
Avoid C++.
Assume normal RISC OS module style.

Reference material

Use the DOSFS source and PRM documentation as references for:

    - registering an image filing system
    - image handler entry points
    - how FileSwitch opens an image file and redirects operations into the image FS
    - catalogue enumeration
    - file open/read/write calls
    - path translation behaviour

But HideousFS is not a FAT/DOS parser. It is a view/proxy filesystem backed by the parent directory of the image file.

Core concept

The image file, conventionally named `Hideous` but selected by filetype `&001` during development:

    HostFS:$.myproject.Hideous

backs onto its containing directory:

    HostFS:$.myproject

The image/config file itself is excluded from the synthetic catalogue by default, regardless of its leafname.

The real files remain in the backing directory. HideousFS maps apparent RISC OS paths to backing HostFS paths.

Operating modes

HideousFS should support two mapping modes, selected by the `Hideous` config file.

1. Hideous mode

    This is the main mode for Unix/GitHub-friendly storage.

    Backing directory contains Unix-style names as seen by RISC OS HostFS, for example:

        leaf/c
        leaf/h
        Readme

    The HideousFS view presents RISC OS-style source layout:

        c.leaf
        h.leaf
        Readme

    This is useful when the same files are also viewed from Unix, where HostFS presents `leaf/c` as `leaf.c`.

2. Beautiful mode

    This is the reverse mode.

    Backing directory contains RISC OS-style names:

        c.leaf
        h.leaf
        Readme

    The HideousFS view presents Unix-style names as seen through HostFS:

        leaf/c
        leaf/h
        Readme

    This does not make the backing directory more useful from Unix, but it is useful for conversion and migration. Copying files out of a Beautiful-mode image view can produce the Unix-friendly layout. It also lets users try the mapping in reverse without immediately changing the original directory structure.

V1 may hard-code Hideous mode. Beautiful mode should be kept in mind when designing the path-mapping functions so that the transform is explicitly reversible.

Initial name mapping

Configured extension mappings should describe a reversible pair. In Hideous mode, Unix-style HostFS names are shown as RISC OS-style source names. In Beautiful mode, the same mapping is used in reverse.

Examples:

    host name        HideousFS name
    ---------        -------------
    leaf/c           c.leaf
    leaf/h           h.leaf
    leaf/s           s.leaf
    leaf/o           o.leaf
    leaf/a           a.leaf
    leaf/cpp         cpp.leaf
    leaf/c++         c++.leaf
    Readme           Readme

The mapping must be reversible.

A simple default mapping table is acceptable initially:

    c     <-> .c
    h     <-> .h
    s     <-> .s
    o     <-> .o
    a     <-> .a
    cpp   <-> .cpp
    c++   <-> .c++

The `Hideous` file should later be parsed as config, including the mode, e.g.:

    # HideousFS
    # filetype &001 during development; leafname is arbitrary
    mode hideous
    reverse c   .c
    reverse h   .h
    reverse s   .s
    reverse o   .o
    reverse a   .a
    reverse cpp .cpp
    reverse c++ .c++
    ignore Hideous
    ignore .git

For reverse conversion, the config may instead say:

    # HideousFS
    # filetype &001 during development; leafname is arbitrary
    mode beautiful
    reverse c   .c
    reverse h   .h
    reverse s   .s
    reverse o   .o
    reverse a   .a
    reverse cpp .cpp
    reverse c++ .c++
    ignore Hideous
    ignore .git

V1 can hard-code the default table before config parsing exists.

Milestones

1. Minimal image FS skeleton

    - Build a C module.
    - Register HideousFS as image filing system number 666 for development.
    - Register/recognise the development image filetype `&001`. Do not rely on the leafname `Hideous`, except as a convenient convention in examples.
    - Opening the image should show a hard-coded catalogue entry such as `Hello`.
    - No backing directory logic yet.

2. Mirror backing directory read-only

    - When `HostFS:$.foo.Hideous` is opened, derive backing directory `HostFS:$.foo`.
    - Catalogue the contents of the backing directory.
    - Exclude the image/config file itself, regardless of its leafname.
    - Initially show names unchanged.
    - Support opening and reading files through FileSwitch from the backing directory.
    - Read-only is sufficient at this stage.

3. Add name transformation

    - In Hideous mode, apply `leaf/c -> c.leaf`, etc. during catalogue enumeration.
    - In Hideous mode, when opening `c.leaf`, map back to `leaf/c`.
    - Keep the mapping code symmetric so Beautiful mode can apply `c.leaf -> leaf/c` during catalogue enumeration and map `leaf/c` back to `c.leaf` for file operations.
    - Unknown names pass through unchanged.
    - Keep collision handling simple initially: either reject ambiguous names or log/report an error.

4. Add writes

    - Saving `c.leaf` should create/update backing file `leaf/c`.
    - Saving `h.foo` should create/update `foo/h`.
    - Saving `Readme` should create/update `Readme`.
    - Creating a file inside a missing synthetic bucket should work; the bucket does not need to have existed before.

5. Directories

    - Real directories in the backing store are real directories and should be shown normally.
    - Creating a normal directory creates a real backing directory.
    - Synthetic extension buckets may be useful later, but should not block v1.

Empty synthetic directories

If the user creates an empty synthetic extension bucket, such as `c`, it is acceptable for v1 to keep that state only in memory.

Suggested rule:

    - Non-empty synthetic buckets appear because matching backing files exist.
    - Empty synthetic buckets can be created, but are volatile.
    - The volatile marker is lost on reboot/module restart.
    - Saving into a missing synthetic bucket still works, because the path can be decoded from the configured mapping.

Example:

   cdir c  (ie. RISC OS's mkdir)

records in module memory:

    backing_dir + bucket "c" is explicitly visible

Then:

    save c.leaf

or equivalent path syntax creates:

    leaf/c

Once at least one `*.c` file exists, the `c` bucket is naturally visible and the volatile marker no longer matters.

Do not persist arbitrary empty synthetic directory state in the `Hideous` file for v1. It can be added later if needed.

Collision handling

Need a defined policy for cases like:

    leaf/c
    c.leaf

Both could appear as `c.leaf`.

For v1, choose one simple policy:

    - transformed names win and raw colliders are hidden, or
    - collisions make one entry appear escaped, or
    - catalogue reports/omits ambiguous entries.

Prefer a simple, visible diagnostic rather than cleverness.

Design constraint

HideousFS is a projection filesystem:

    - Backing storage is the containing HostFS directory.
    - The image file is a portal/config file.
    - The image file is identified by filetype, not by the leafname `Hideous`.
    - File operations are proxied to real files through FileSwitch.
    - The same mapping table should work in both directions, with the config-selected mode deciding which direction is used for catalogue presentation and which direction is used for backing-store operations.

Read-only first. Add writes after catalogue/open/read are reliable.

Recommended first target

Produce a C module that lets this work:

    HostFS:$.myproject.Hideous

and opening it shows a read-only catalogue containing:

    Hello

Then replace `Hello` with the real backing directory enumeration.


Compiler

It should be compiled on macOS using 'ncc-riscos'. Other tools are 'cmunge' which is a replacement for Acorn's 'cmhg', and processes files with that extensions. 'drlink' should be used to link the objects, with 'stubs.a'.

See DOSFS (up one directory) as an example, though that compilers under RISC OS. See PDrivers (also up one directory) as an example of a set of four modules that compile under macOS using the named tools.

Repository documents

Keep this file as the developer/implementation specification. Add a separate `README.md` for user-facing documentation: what HideousFS does, how to create a config image file, how Hideous and Beautiful modes behave, and the current temporary nature of filetype `&001` and filing system number 666.
