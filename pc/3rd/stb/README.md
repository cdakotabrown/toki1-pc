# stb (vendored)

`stb_image.h` (v2.30) and `stb_image_write.h` (v1.16) by Sean Barrett,
<https://github.com/nothings/stb>, unmodified upstream copies.  Both are
dual-licensed **public domain (Unlicense) or MIT** (the licence text is at
the end of each file).

Used by `pc/hal/gpu_texrep.c` only (texture dumping and replacement packs:
PNG read and write), compiled there with `STB_IMAGE_STATIC` /
`STB_IMAGE_WRITE_STATIC` and `STBI_ONLY_PNG`, so no symbol leaves that
object.
