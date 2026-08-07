# Windows portability regression expectations

Date: 2026-08-07

These are post-specified regression checks written after the original Windows
portability implementation and its first validation runs, but before the two
review fixes and before running either new check. They are not a public
pre-registration of the original pull request.

## Behavioral case: binary GOAL rank count

The little-endian `num_ranks` value 26 places byte `0x1A` immediately after
the eight-byte magic cookie. The CMake-built `txt2bin` must preserve the exact
header bytes `1a000000`. `htsim_rnic` must read that byte as binary data, expose
26 ranks under `gpu-rank` mapping, complete one 1024-byte flow from rank 0 to
rank 25 on `rnic-nn-fluid` at 400 Gb/s, exit successfully, write exactly one
completion row, and report `physical_quiescence=verified` on Linux and native
MSVC Windows.

The reviewed PR head is expected to pass this case on Linux and fail on native
Windows because `Parser` opens the binary schedule in translated text mode,
where `0x1A` is an end-of-file marker.

## Structural invariant: source-tree cleanliness

A default build of the CMake `txt2bin` target must leave the tracked
`htsim/sim/lgs/txt2bin` source artifact as the same regular file with the same
SHA-256 observed at configure time. Callers use the executable in the build
tree. The build must not replace the tracked source artifact with a symlink.

The reviewed PR head is expected to fail this invariant on Unix, where source
symlinks default on, and pass it on Windows, where they default off.

## Evidence accounting

The 344 existing native tests remain their own evidence class. Report the new
binary round-trip as one behavioral case and the source-tree check as one
unscored structural invariant. Do not add those counts into one behavioral
headline.
