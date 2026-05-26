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

The program leaves the generated test tree in place so it can be inspected
after a failure.
