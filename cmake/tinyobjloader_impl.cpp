// tinyobjloader is header-only; exactly one translation unit defines the
// implementation. The Saffron.Geometry module includes the header (declarations
// only) and uses the bool-returning LoadObj overload (no exceptions escape).
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
