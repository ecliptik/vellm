# CWSDPMI r7 (vendored binary)

**Source:** <http://www.delorie.com/pub/djgpp/current/v2misc/csdpmi7b.zip>
(the `b` suffix = binary-only distribution).

**Version:** Release 7, build-dated 2010-01-07 (author: Charles W. Sandmann,
`cwsdpmi@earthlink.net`).

**Files in this directory:**

| File           | Origin                     | Purpose                                    |
|----------------|----------------------------|--------------------------------------------|
| `cwsdpmi.exe`  | `bin/CWSDPMI.EXE` from zip | DPMI host; required at runtime next to `vellm.exe` |
| `cwsdpmi.doc`  | `bin/cwsdpmi.doc` from zip | Upstream readme + license terms            |
| `README.md`    | this repo                  | Provenance + redistribution notes          |

## Why it's here

DJGPP programs use DPMI (DOS Protected-Mode Interface) at runtime. Real-mode
MS-DOS does not provide DPMI on its own (unlike the DOS subsystem under
Windows), so a host must be loaded first. CWSDPMI is the canonical free DPMI
host shipped with DJGPP.

On the target Pentium Overdrive box we cannot rely on the user to have
CWSDPMI installed, so we ship it alongside `vellm.exe`. The `make install`
and `make dist` targets both copy `cwsdpmi.exe` into the deploy payload.

## License / redistribution

From `cwsdpmi.doc`:

> CWSDPMI is Copyright (C) 1995-2010  Charles W Sandmann.
> The files in this binary distribution may be redistributed under the GPL
> (with source) or without the source code provided:
>
> * CWSDPMI.EXE or CWSDPR0.EXE are not modified in any way except via CWSPARAM.
> * CWSDSTUB.EXE internal contents are not modified in any way except via
>   CWSPARAM or STUBEDIT. It may have a COFF image plus data appended to it.
> * Notice to users that they have the right to receive the source code and/or
>   binary updates for CWSDPMI. Distributors should indicate a site for the
>   source in their documentation.

We ship `cwsdpmi.exe` unmodified. Users have the right to obtain the source
and updates from the DJGPP archives at
<http://www.delorie.com/pub/djgpp/current/v2misc/> (look for `csdpmi7s.zip`
for the source distribution).

## Refreshing

Re-download the zip from the URL above, replace `cwsdpmi.exe` and
`cwsdpmi.doc`, and commit. Do not modify the binary.
