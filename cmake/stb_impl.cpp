// The stb single-header libraries are header-only; exactly one translation unit
// defines each implementation. Consumers include the headers (declarations only):
// the renderer writes PNGs, the geometry module decodes textures.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
