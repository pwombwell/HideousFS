# HideousFS

HideousFS is a RISC OS image filing system that presents a transformed view of a directory.

It is intended for source trees that need to be comfortable both on RISC OS and on Unix-like systems. Unix and GitHub prefer names such as `leaf.c`; RISC OS source conventions often use names such as `c.leaf`. HideousFS lets one layout be viewed as the other.

## Image file

A HideousFS image file is a small configuration file placed inside the directory to be viewed. The file data are not stored inside the image file; the real files remain in the surrounding directory.

The example name is usually:

```text
View
```

but the leafname is arbitrary. HideousFS should identify the image by filetype, not by name.

During development the image filetype is:

```text
&001
```

This is a temporary user-area filetype and may clash with existing uses such as music files.

![Screenshot of HideousFS directory view](docs/screenshot.png)

## Hideous mode

Hideous mode is for Unix/GitHub-friendly backing stores.

Backing directory as seen through HostFS:

```text
leaf/c
leaf/h
Readme
```

The same files are typically seen from Unix as:

```text
leaf.c
leaf.h
Readme
```

HideousFS presents them to RISC OS as:

```text
c.leaf
h.leaf
Readme
```

## Beautiful mode

Beautiful mode is the reverse view.

Backing directory:

```text
c.leaf
h.leaf
Readme
```

HideousFS presents:

```text
leaf/c
leaf/h
Readme
```

This is mainly useful for conversion. Copying files out of a Beautiful-mode view can produce a Unix-friendly layout.

## Example configuration

```text
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
ignore View
ignore .git
```

For the reverse view:

```text
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
ignore View
ignore .git
```

## Status

HideousFS is experimental. The initial target is a read-only image filing system that can open a config file and show a transformed catalogue of the surrounding directory. Writes, renames, collision handling, and persistent empty synthetic directories can be added once the basic read path is reliable.
```
