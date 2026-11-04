# LodePNG

The PNG decoder for Satellite mode's PNG button images (`BITMAP_FORMAT=png`), wrapped by `../png.c`.

- Source: <https://github.com/lvandeve/lodepng>, commit `e584a8490db1b76b81bf298c11c09d3b55d9c270`
  (version 20261006).
- Unmodified, except that `lodepng.cpp` is named `lodepng.c` here (LodePNG documents building it as C that way).
- Built as a decoder only: `LODEPNG_NO_COMPILE_ENCODER`, `LODEPNG_NO_COMPILE_DISK`, `LODEPNG_NO_COMPILE_CPP`,
  `LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS`, `LODEPNG_NO_COMPILE_ERROR_TEXT` (`../CMakeLists.txt`, and the host tests).
- Licence: zlib, in [LICENSE](LICENSE). Copyright (c) 2005-2018 Lode Vandevenne.
