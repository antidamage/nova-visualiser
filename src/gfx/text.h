#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nova::gfx {

// Rasterised centre message.
//
// Two surfaces rather than one, because the two halves are coloured
// differently: text is a coverage mask tinted per frame by the chased palette
// (so a theme change costs no re-rasterisation), while emoji carry their own
// colour and must be composited as-is. Packing both into one RGBA texture would
// make a black emoji pixel indistinguishable from a text pixel.
struct RasterizedText {
  int width = 0;
  int height = 0;
  // Coverage, one byte per pixel. Text glyphs only.
  std::vector<uint8_t> coverage;
  // Premultiplied RGBA. Colour emoji glyphs only.
  std::vector<uint8_t> color;
  bool hasColor = false;
};

// Wraps FreeType. Loads a text face and a colour-emoji face and lays a message
// out centred in a frame of the given size, falling back to the emoji face for
// any codepoint the text face has no glyph for.
class TextRasterizer {
 public:
  TextRasterizer();
  ~TextRasterizer();

  TextRasterizer(const TextRasterizer&) = delete;
  TextRasterizer& operator=(const TextRasterizer&) = delete;

  // Both paths are optional: a missing emoji face degrades to text-only rather
  // than failing the renderer, and a missing text face disables the message
  // instead of taking the stream down.
  bool load(const std::string& textFontPath, const std::string& emojiFontPath, std::string& error);
  bool ready() const { return textFace_ != nullptr; }

  // `pixelSize` is the text em size in device pixels. `maxWidth` bounds the
  // wrapped line box.
  RasterizedText render(const std::string& message, int width, int height, int pixelSize,
                        int maxWidth) const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  void* textFace_ = nullptr;
  void* emojiFace_ = nullptr;
};

}  // namespace nova::gfx
