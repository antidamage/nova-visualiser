#include "gfx/text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace nova::gfx {
namespace {

std::vector<uint32_t> decodeUtf8(const std::string& source) {
  std::vector<uint32_t> decoded;
  decoded.reserve(source.size());
  size_t index = 0;
  while (index < source.size()) {
    const uint8_t first = static_cast<uint8_t>(source[index]);
    uint32_t codepoint = 0xfffd;
    size_t count = 1;
    if (first < 0x80) {
      codepoint = first;
    } else if ((first & 0xe0) == 0xc0 && index + 1 < source.size()) {
      codepoint = static_cast<uint32_t>(first & 0x1f) << 6;
      codepoint |= static_cast<uint8_t>(source[index + 1]) & 0x3f;
      count = 2;
    } else if ((first & 0xf0) == 0xe0 && index + 2 < source.size()) {
      codepoint = static_cast<uint32_t>(first & 0x0f) << 12;
      codepoint |= static_cast<uint32_t>(static_cast<uint8_t>(source[index + 1]) & 0x3f) << 6;
      codepoint |= static_cast<uint8_t>(source[index + 2]) & 0x3f;
      count = 3;
    } else if ((first & 0xf8) == 0xf0 && index + 3 < source.size()) {
      codepoint = static_cast<uint32_t>(first & 0x07) << 18;
      codepoint |= static_cast<uint32_t>(static_cast<uint8_t>(source[index + 1]) & 0x3f) << 12;
      codepoint |= static_cast<uint32_t>(static_cast<uint8_t>(source[index + 2]) & 0x3f) << 6;
      codepoint |= static_cast<uint8_t>(source[index + 3]) & 0x3f;
      count = 4;
    }
    index += count;
    // Variation selectors pick a presentation, they are not visible cells. Zero
    // width joiners are dropped too: without a shaping engine the parts of a
    // ZWJ sequence render as their individual emoji, which is a far better
    // failure than a tofu box.
    if (codepoint == 0xfe0f || codepoint == 0xfe0e || codepoint == 0x200d) continue;
    decoded.push_back(codepoint);
  }
  return decoded;
}

}  // namespace

struct TextRasterizer::Impl {
  FT_Library library = nullptr;
  FT_Face text = nullptr;
  FT_Face emoji = nullptr;
  // Apple Color Emoji is a CBDT bitmap font: it has fixed strikes rather than
  // scalable outlines, so glyphs come out at the strike size and are resampled
  // to the line size here.
  int emojiStrikeSize = 0;
};

TextRasterizer::TextRasterizer() : impl_(new Impl()) {}

TextRasterizer::~TextRasterizer() {
  if (impl_ != nullptr) {
    if (impl_->text != nullptr) FT_Done_Face(impl_->text);
    if (impl_->emoji != nullptr) FT_Done_Face(impl_->emoji);
    if (impl_->library != nullptr) FT_Done_FreeType(impl_->library);
    delete impl_;
  }
}

bool TextRasterizer::load(const std::string& textFontPath, const std::string& emojiFontPath,
                          std::string& error) {
  if (FT_Init_FreeType(&impl_->library) != 0) {
    error = "FreeType initialisation failed";
    return false;
  }
  if (FT_New_Face(impl_->library, textFontPath.c_str(), 0, &impl_->text) != 0) {
    error = "could not open text font " + textFontPath;
    impl_->text = nullptr;
    return false;
  }
  textFace_ = impl_->text;

  // Optional. A stream with plain text beats no stream at all.
  if (!emojiFontPath.empty() &&
      FT_New_Face(impl_->library, emojiFontPath.c_str(), 0, &impl_->emoji) == 0) {
    emojiFace_ = impl_->emoji;
    int best = -1;
    for (int index = 0; index < impl_->emoji->num_fixed_sizes; ++index) {
      const int size = impl_->emoji->available_sizes[index].height;
      if (best < 0 || size > impl_->emoji->available_sizes[best].height) best = index;
    }
    if (best >= 0) {
      FT_Select_Size(impl_->emoji, best);
      impl_->emojiStrikeSize = impl_->emoji->available_sizes[best].height;
    } else {
      FT_Done_Face(impl_->emoji);
      impl_->emoji = nullptr;
      emojiFace_ = nullptr;
    }
  }
  return true;
}

RasterizedText TextRasterizer::render(const std::string& message, int width, int height,
                                      int pixelSize, int maxWidth) const {
  RasterizedText out;
  out.width = std::max(1, width);
  out.height = std::max(1, height);
  out.coverage.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 0);
  if (impl_->text == nullptr || message.empty()) return out;

  FT_Set_Pixel_Sizes(impl_->text, 0, static_cast<FT_UInt>(std::max(8, pixelSize)));
  const int lineHeight = static_cast<int>(impl_->text->size->metrics.height >> 6);
  const int ascender = static_cast<int>(impl_->text->size->metrics.ascender >> 6);
  // Emoji are sized to the text's cap height rather than the em box so they sit
  // visually level with the letters instead of towering over them.
  const int emojiSize = static_cast<int>(std::lround(pixelSize * 1.15));

  struct Cell {
    uint32_t codepoint = 0;
    bool emoji = false;
    int advance = 0;
  };

  const std::vector<uint32_t> codepoints = decodeUtf8(message);
  std::vector<Cell> cells;
  cells.reserve(codepoints.size());
  for (uint32_t codepoint : codepoints) {
    Cell cell;
    cell.codepoint = codepoint;
    const FT_UInt textGlyph = FT_Get_Char_Index(impl_->text, codepoint);
    if (textGlyph == 0 && impl_->emoji != nullptr &&
        FT_Get_Char_Index(impl_->emoji, codepoint) != 0) {
      cell.emoji = true;
      cell.advance = emojiSize + std::max(1, emojiSize / 12);
    } else if (textGlyph != 0) {
      if (FT_Load_Glyph(impl_->text, textGlyph, FT_LOAD_DEFAULT) == 0) {
        cell.advance = static_cast<int>(impl_->text->glyph->advance.x >> 6);
      }
    } else {
      // No glyph anywhere. Skip rather than drawing tofu.
      continue;
    }
    cells.push_back(cell);
  }
  if (cells.empty()) return out;

  // Greedy wrap on spaces, matching the SwiftUI overlay's three-line limit.
  const int limit = std::max(pixelSize * 4, maxWidth);
  std::vector<std::vector<Cell>> lines;
  std::vector<Cell> line;
  std::vector<Cell> word;
  int lineWidth = 0;
  int wordWidth = 0;
  auto flushWord = [&]() {
    if (word.empty()) return;
    if (!line.empty() && lineWidth + wordWidth > limit) {
      lines.push_back(line);
      line.clear();
      lineWidth = 0;
    }
    line.insert(line.end(), word.begin(), word.end());
    lineWidth += wordWidth;
    word.clear();
    wordWidth = 0;
  };
  for (const Cell& cell : cells) {
    if (cell.codepoint == '\n') {
      flushWord();
      lines.push_back(line);
      line.clear();
      lineWidth = 0;
      continue;
    }
    word.push_back(cell);
    wordWidth += cell.advance;
    if (cell.codepoint == ' ') flushWord();
  }
  flushWord();
  if (!line.empty()) lines.push_back(line);
  if (lines.size() > 3) lines.resize(3);

  const int blockHeight = static_cast<int>(lines.size()) * lineHeight;
  int penY = (out.height - blockHeight) / 2 + ascender;

  auto blendCoverage = [&](int x, int y, uint8_t value) {
    if (x < 0 || y < 0 || x >= out.width || y >= out.height || value == 0) return;
    uint8_t& target = out.coverage[static_cast<size_t>(y) * static_cast<size_t>(out.width) +
                                   static_cast<size_t>(x)];
    target = static_cast<uint8_t>(std::min(255, static_cast<int>(target) + value));
  };

  for (const std::vector<Cell>& row : lines) {
    int rowWidth = 0;
    for (const Cell& cell : row) rowWidth += cell.advance;
    int penX = (out.width - rowWidth) / 2;

    for (const Cell& cell : row) {
      if (cell.emoji && impl_->emoji != nullptr) {
        if (FT_Load_Char(impl_->emoji, cell.codepoint, FT_LOAD_COLOR | FT_LOAD_RENDER) == 0 &&
            impl_->emoji->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
          if (!out.hasColor) {
            out.color.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4,
                             0);
            out.hasColor = true;
          }
          const FT_Bitmap& bitmap = impl_->emoji->glyph->bitmap;
          const int sourceWidth = static_cast<int>(bitmap.width);
          const int sourceHeight = static_cast<int>(bitmap.rows);
          // Box-filtered downscale from the strike to the line size. The strike
          // is 160px and the line is usually smaller, so point sampling would
          // alias badly on a 4K encode.
          const int targetSize = emojiSize;
          const int originX = penX;
          const int originY = penY - static_cast<int>(std::lround(targetSize * 0.82));
          for (int y = 0; y < targetSize; ++y) {
            const int y0 = y * sourceHeight / targetSize;
            const int y1 = std::max(y0 + 1, (y + 1) * sourceHeight / targetSize);
            for (int x = 0; x < targetSize; ++x) {
              const int x0 = x * sourceWidth / targetSize;
              const int x1 = std::max(x0 + 1, (x + 1) * sourceWidth / targetSize);
              int b = 0;
              int g = 0;
              int r = 0;
              int a = 0;
              int samples = 0;
              for (int sy = y0; sy < y1 && sy < sourceHeight; ++sy) {
                const uint8_t* scan = bitmap.buffer + static_cast<ptrdiff_t>(sy) * bitmap.pitch;
                for (int sx = x0; sx < x1 && sx < sourceWidth; ++sx) {
                  b += scan[sx * 4 + 0];
                  g += scan[sx * 4 + 1];
                  r += scan[sx * 4 + 2];
                  a += scan[sx * 4 + 3];
                  ++samples;
                }
              }
              if (samples == 0 || a == 0) continue;
              const int destX = originX + x;
              const int destY = originY + y;
              if (destX < 0 || destY < 0 || destX >= out.width || destY >= out.height) continue;
              const size_t offset =
                  (static_cast<size_t>(destY) * static_cast<size_t>(out.width) +
                   static_cast<size_t>(destX)) * 4;
              // FreeType hands back premultiplied BGRA; the texture is RGBA.
              out.color[offset + 0] = static_cast<uint8_t>(r / samples);
              out.color[offset + 1] = static_cast<uint8_t>(g / samples);
              out.color[offset + 2] = static_cast<uint8_t>(b / samples);
              out.color[offset + 3] = static_cast<uint8_t>(a / samples);
            }
          }
        }
        penX += cell.advance;
        continue;
      }

      if (FT_Load_Char(impl_->text, cell.codepoint, FT_LOAD_RENDER) != 0) {
        penX += cell.advance;
        continue;
      }
      const FT_GlyphSlot glyph = impl_->text->glyph;
      const FT_Bitmap& bitmap = glyph->bitmap;
      if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (int y = 0; y < static_cast<int>(bitmap.rows); ++y) {
          const uint8_t* scan = bitmap.buffer + static_cast<ptrdiff_t>(y) * bitmap.pitch;
          for (int x = 0; x < static_cast<int>(bitmap.width); ++x) {
            blendCoverage(penX + glyph->bitmap_left + x, penY - glyph->bitmap_top + y, scan[x]);
          }
        }
      }
      penX += cell.advance;
    }
    penY += lineHeight;
  }

  return out;
}

}  // namespace nova::gfx
