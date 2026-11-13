# Audio decoders (vendored, single-file)

Used only by the music packs (`pc/hal/music_decode.c`, enhanced profile).  `dr_flac.h` and
`dr_wav.h` are unmodified from the upstream revision below; `stb_vorbis.c` carries this
project's fixes for the 2023 GitHub Security Lab reports, which upstream has not merged
(GHSL-2023-165..170, CVE-2023-45675..45680: header lengths and counts that wrap the
allocation size, and frees of unset pointers after a failed allocation), each marked
`[toki1-pc]` in the file.  A music pack is a file a player may download, so its decoder
is untrusted input.  `pc/hal/music_decode.c` compiles their implementations into one
translation unit.

| File | Version | Upstream revision | Licence |
| --- | --- | --- | --- |
| `stb_vorbis.c` | v1.22 | nothings/stb `d14d46f6b44456eb88beac58cba833a4135d4c89` | public domain (Unlicense) or MIT, the reader's choice (end of the file) |
| `dr_flac.h` | v0.13.3 | mackron/dr_libs `66d73f587b1ab9b8a098d0a514fe6c56fffa86ae` | public domain (Unlicense) or MIT-0 (end of the file) |
| `dr_wav.h` | v0.14.5 | mackron/dr_libs `66d73f587b1ab9b8a098d0a514fe6c56fffa86ae` | public domain (Unlicense) or MIT-0 (end of the file) |

SHA-256 (of the upstream files; `stb_vorbis.c` here differs by the fixes above):

```
4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636  stb_vorbis.c
a35468f278f17cf9538ce189debf76a5854a77e139980f7b5185d67566a0c837  dr_flac.h
0555e17317a76bbe25593c0a683f3fb177f73380350b8f5d28d905de48810a52  dr_wav.h
```

To fetch them again:
`curl -O https://raw.githubusercontent.com/nothings/stb/<rev>/stb_vorbis.c`,
`curl -O https://raw.githubusercontent.com/mackron/dr_libs/<rev>/dr_flac.h`
(and `dr_wav.h`).
