idf_component_register(SRCS transform.c bmp.c jpeg_enc_hw.c jpeg_dec_hw.c png.c lodepng/lodepng.c
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS lodepng
                       REQUIRES esp_driver_jpeg
                       PRIV_REQUIRES log)
# LodePNG (third party, zlib licence, lodepng/README.md): the PNG decoder only; its own code, its own warnings
target_compile_definitions(${COMPONENT_LIB} PRIVATE LODEPNG_NO_COMPILE_ENCODER LODEPNG_NO_COMPILE_DISK
                           LODEPNG_NO_COMPILE_CPP LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS LODEPNG_NO_COMPILE_ERROR_TEXT)
set_source_files_properties(lodepng/lodepng.c PROPERTIES COMPILE_OPTIONS "-Wno-error")
