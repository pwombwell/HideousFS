# HideousFS Tests

This directory contains RISC OS-side smoke tests. They are intended to run
under RISC OS, with the HideousFS module already loaded.

## Smoke,ffb

`Smoke,ffb` is a plain text BBC BASIC program. It creates a temporary HostFS
test tree, writes Hideous and Beautiful image config files, sets them to
filetype `&001`, catalogues both image views, and checks a few write paths.

Run it from BASIC, or load it into your editor and run it there. The default
test root is:

```text
HostFS:$.HideousFSTest<time>
```

Edit line 40 if you want a different temporary location.

The current assertions check:

- Hideous mode can save `c.newhide` through the image view and create backing
  file `newhide/c`.
- Beautiful mode starts with no `h` backing bucket.
- Beautiful mode can save `header/h` through the image view and create backing
  path `h.header`.
- Hideous mode can rename `c.main` to `c.renamed` and move backing
  `main/c` to `renamed/c`.
- Beautiful mode can rename `main/c` to `renamed/c` and move backing
  `c.main` to `c.renamed`.
- Projected and pass-through deletes remove the expected backing objects.
- Projected files in both modes can have their filetype set to BASIC
  (`&FFB`) through the image view, and the backing objects report the same
  filetype.
- Two image files in the same backing directory can use different `reverse`
  lists, and each view only maps the extension configured by that image.
- Projected files inside a real subdirectory are readable and new projected
  saves inside that subdirectory create the expected backing paths.
- Hideous and Beautiful collision cases read the projected backing object when
  a raw backing object could otherwise appear at the same image path.
- Single-byte reads after setting `PTR#`, bulk `OS_GBPB` reads, and bulk
  `OS_GBPB` writes through projected paths behave as expected.
- A projected file written through `OS_GBPB` reports the expected extent.

The program leaves the generated test tree in place so it can be inspected
after a failure.
