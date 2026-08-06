#pragma once

#include <memory>
#include <string>

#include "core/image.h"

namespace nova::gfx {

// Decodes a PNG into premultiplied RGBA8.
//
// libpng via pkg-config rather than a vendored single-header decoder, matching
// how FreeType is taken for the centre message: the dependency is declared, the
// build fails loudly without it, and correctness for palettes, tRNS, 16-bit and
// interlaced files is somebody else's already-solved problem.
//
// Lives in the GPU half deliberately. `nova_visualiser_core` links nothing but
// Threads so the conformance runner builds on a machine with no GPU and no image
// libraries; only the struct it carries is in core.
//
// Returns null and fills `error` on anything malformed. A centre image that
// cannot be decoded is a missing centre image, never a dead stream.
std::shared_ptr<const DecodedImage> decodePng(const std::string& bytes,
                                              const std::string& sourceUrl,
                                              std::string& error);

}  // namespace nova::gfx
