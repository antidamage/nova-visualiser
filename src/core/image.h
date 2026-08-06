#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nova {

// A decoded centre image, ready to upload.
//
// Deliberately a struct with no decoder attached: `nova_visualiser_core` links
// nothing but Threads so the conformance runner builds on a machine with no GPU
// and no image libraries, and the simulation only ever needs to carry one of
// these around and compare identity. The decoding lives in
// `src/gfx/image_decode.h`, which is part of the GPU half.
//
// RGBA is premultiplied at decode time, because the centre pass blends
// GL_ONE / GL_ONE_MINUS_SRC_ALPHA -- the same convention the colour-emoji plane
// of the message already arrives in.
struct DecodedImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
  // Where it came from, which is also its cache key. Two images are the same
  // image when they are the same pointer, so the renderer never compares pixels
  // or re-hashes a multi-megabyte buffer per frame.
  std::string sourceUrl;
};

}  // namespace nova
