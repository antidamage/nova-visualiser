#include "gfx/image_decode.h"

#include <png.h>

#include <cstring>
#include <vector>

namespace nova::gfx {
namespace {

struct Reader {
  const std::string* bytes = nullptr;
  size_t offset = 0;
};

void readFromMemory(png_structp png, png_bytep out, png_size_t count) {
  auto* reader = static_cast<Reader*>(png_get_io_ptr(png));
  if (reader == nullptr || reader->bytes == nullptr ||
      reader->offset + count > reader->bytes->size()) {
    png_error(png, "read past end of PNG");
    return;
  }
  std::memcpy(out, reader->bytes->data() + reader->offset, count);
  reader->offset += count;
}

// libpng reports recoverable problems through here. Silence rather than stderr:
// a slightly non-conforming PNG that still decodes is not something to shout
// about every time the config is re-read.
void onWarning(png_structp, png_const_charp) {}

}  // namespace

std::shared_ptr<const DecodedImage> decodePng(const std::string& bytes,
                                              const std::string& sourceUrl,
                                              std::string& error) {
  if (png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0,
                  bytes.size() < 8 ? bytes.size() : 8) != 0) {
    error = "not a PNG";
    return nullptr;
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, onWarning);
  if (png == nullptr) {
    error = "png_create_read_struct failed";
    return nullptr;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    error = "png_create_info_struct failed";
    return nullptr;
  }

  // libpng's error path is a longjmp, so everything it can jump over has to be
  // destroyed by hand here rather than left to a destructor.
  std::vector<png_bytep> rows;
  auto image = std::make_shared<DecodedImage>();
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    error = "malformed PNG";
    return nullptr;
  }

  Reader reader{&bytes, 0};
  png_set_read_fn(png, &reader, readFromMemory);
  png_read_info(png, info);

  const png_uint_32 width = png_get_image_width(png, info);
  const png_uint_32 height = png_get_image_height(png, info);
  if (width == 0 || height == 0 || width > 8192 || height > 8192) {
    png_destroy_read_struct(&png, &info, nullptr);
    error = "PNG dimensions out of range";
    return nullptr;
  }

  // Normalise every colour type, bit depth and transparency form to 8-bit RGBA
  // in one pass, so nothing downstream has to know a palette from a greyscale.
  const png_byte colorType = png_get_color_type(png, info);
  const png_byte bitDepth = png_get_bit_depth(png, info);
  if (bitDepth == 16) png_set_strip_16(png);
  if (colorType == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) png_set_tRNS_to_alpha(png);
  if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }
  // An opaque PNG is legitimate -- somebody's logo on a solid card -- so a
  // missing alpha channel is filled rather than rejected.
  png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_set_interlace_handling(png);
  png_read_update_info(png, info);

  image->width = static_cast<int>(width);
  image->height = static_cast<int>(height);
  image->sourceUrl = sourceUrl;
  image->rgba.assign(static_cast<size_t>(width) * height * 4, 0);

  rows.resize(height);
  for (png_uint_32 row = 0; row < height; ++row) {
    rows[row] = image->rgba.data() + static_cast<size_t>(row) * width * 4;
  }
  png_read_image(png, rows.data());
  png_destroy_read_struct(&png, &info, nullptr);

  // Premultiply. The centre pass blends GL_ONE / GL_ONE_MINUS_SRC_ALPHA, the
  // same convention the message's colour-emoji plane already arrives in, and
  // doing it once here costs nothing per frame.
  for (size_t index = 0; index + 3 < image->rgba.size(); index += 4) {
    const uint32_t alpha = image->rgba[index + 3];
    if (alpha == 255) continue;
    for (int channel = 0; channel < 3; ++channel) {
      image->rgba[index + channel] =
          static_cast<uint8_t>((image->rgba[index + channel] * alpha + 127) / 255);
    }
  }

  return image;
}

}  // namespace nova::gfx
