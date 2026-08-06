#include "gfx/renderer.h"

#include "core/centre_image_reference.h"
#include "core/effect_scale.h"

#include <epoxy/gl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "embedded_shaders.h"
#include "gfx/gl_util.h"

namespace nova::gfx {
namespace {

struct GpuUniforms {
  float viewport[4];
  float boundsMin[4];
  float boundsMax[4];
  float signalData[4];
  float blend[4];
};

constexpr int kSceneBinding = 0;
constexpr int kCurrentBinding = 0;
constexpr int kPreviousBinding = 1;
constexpr int kUniformBinding = 2;

// Bloom upsample blend weight.
//
// This used to be a plain additive accumulation (GL_ONE, GL_ONE), which is
// wrong for a progressive mip chain: every destination mip already holds its
// own downsampled content, so each of the five upsample steps stacked another
// full copy and mip 0 came out at roughly 5x the bright pass. The composite
// then multiplied that by an intensity borrowed verbatim from tvOS -- where the
// bloom is a SINGLE quarter-resolution blur with normalised weights -- so the
// streamed image carried about seven times the intended glow, spread all the
// way out to a 120x67 mip. At 4K that is a screen-wide wash: after the encode
// pass's Reinhard tonemap it flattens into a pale field with only the sharpest
// particle cores and grid wires punching through, which is exactly what the
// television was showing.
//
// Lerping instead of adding keeps the chain energy-preserving: each step is
// `mix(destination, upsampled, kBloomScatter)`, so mip 0 lands at the bright
// pass's magnitude no matter how many mips the chain has. 0.5 weights every
// octave equally; lower values bias towards fine detail and a tighter bloom.
constexpr float kBloomScatter = 0.5f;

// Composite glow gain. Meaningful only against an energy-preserving chain --
// see kBloomScatter. Matches the tvOS constant because the two chains now carry
// the same total energy, though this one spreads it over more octaves.
constexpr float kCompositeIntensity = 1.45f;

// Continuous, render-rate-independent clock for the backdrop. Deliberately not
// the signal clock: the backdrop must keep drifting while playback is paused or
// between tracks, exactly as it does on tvOS.
double backdropSeconds() {
  static const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
}

}  // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::initialise(int width, int height, std::string& error) {
  width_ = width;
  height_ = height;
  renderWidth_ = width;
  renderHeight_ = height;

  auto raster = [&](uint32_t& program, std::string_view name, std::string_view vertex,
                    std::string_view fragment) {
    program = buildProgram(vertex, fragment, error);
    if (program == 0) error = std::string(name) + ": " + error;
    return program != 0;
  };
  auto compute = [&](uint32_t& program, std::string_view name, std::string_view source) {
    program = buildComputeProgram(source, error);
    if (program == 0) error = std::string(name) + ": " + error;
    return program != 0;
  };

  if (!raster(particleProgram_, "particle", shaders::particle_vert, shaders::particle_frag)) {
    return false;
  }
  if (!raster(extractProgram_, "bloom_extract", shaders::fullscreen_vert,
              shaders::bloom_extract_frag)) {
    return false;
  }
  if (!raster(downsampleProgram_, "bloom_downsample", shaders::fullscreen_vert,
              shaders::bloom_downsample_frag)) {
    return false;
  }
  if (!raster(upsampleProgram_, "bloom_upsample", shaders::fullscreen_vert,
              shaders::bloom_upsample_frag)) {
    return false;
  }
  if (!raster(compositeProgram_, "composite", shaders::fullscreen_vert, shaders::composite_frag)) {
    return false;
  }
  if (!raster(fluidProgram_, "fluid_background", shaders::fullscreen_vert,
              shaders::fluid_background_frag)) {
    return false;
  }
  if (!raster(textProgram_, "text_overlay", shaders::fullscreen_vert,
              shaders::text_overlay_frag)) {
    return false;
  }
  if (!raster(centreImageProgram_, "centre_image", shaders::fullscreen_vert,
              shaders::centre_image_frag)) {
    return false;
  }
  if (!raster(glowBlurProgram_, "glow_blur", shaders::fullscreen_vert, shaders::glow_blur_frag)) {
    return false;
  }
  if (!raster(glowOverlayProgram_, "glow_overlay", shaders::fullscreen_vert,
              shaders::glow_overlay_frag)) {
    return false;
  }
  if (!compute(p010Program_, "encode_p010", shaders::encode_p010_comp)) return false;
  if (!compute(nv12Program_, "encode_nv12", shaders::encode_nv12_comp)) return false;

  compositeIntensity_ = glGetUniformLocation(compositeProgram_, "intensity");
  compositeBackground_ = glGetUniformLocation(compositeProgram_, "background");
  compositeUseFluid_ = glGetUniformLocation(compositeProgram_, "useFluid");
  compositeBlendMode_ = glGetUniformLocation(compositeProgram_, "sceneBlendMode");
  fluidUniforms_.bandResolution = glGetUniformLocation(fluidProgram_, "bandResolution");
  fluidUniforms_.bandFraction = glGetUniformLocation(fluidProgram_, "bandFraction");
  fluidUniforms_.bandWidthFraction = glGetUniformLocation(fluidProgram_, "bandWidthFraction");
  fluidUniforms_.vignetteColor = glGetUniformLocation(fluidProgram_, "vignetteColor");
  fluidUniforms_.vignetteOpacity = glGetUniformLocation(fluidProgram_, "vignetteOpacity");
  fluidUniforms_.vignetteSize = glGetUniformLocation(fluidProgram_, "vignetteSize");
  fluidUniforms_.time = glGetUniformLocation(fluidProgram_, "time");
  fluidUniforms_.background = glGetUniformLocation(fluidProgram_, "background");
  fluidUniforms_.accent = glGetUniformLocation(fluidProgram_, "accent");
  fluidUniforms_.highlight = glGetUniformLocation(fluidProgram_, "highlight");
  fluidUniforms_.peakIntensity = glGetUniformLocation(fluidProgram_, "peakIntensity");
  fluidUniforms_.falloffPower = glGetUniformLocation(fluidProgram_, "falloffPower");
  fluidUniforms_.warpAmplitude = glGetUniformLocation(fluidProgram_, "warpAmplitude");
  fluidUniforms_.hueSpread = glGetUniformLocation(fluidProgram_, "hueSpread");
  fluidUniforms_.apexGlow = glGetUniformLocation(fluidProgram_, "apexGlow");
  fluidUniforms_.blobScale = glGetUniformLocation(fluidProgram_, "blobScale");
  fluidUniforms_.blobSoftness = glGetUniformLocation(fluidProgram_, "blobSoftness");
  downsampleTexel_ = glGetUniformLocation(downsampleProgram_, "texelSize");
  upsampleTexel_ = glGetUniformLocation(upsampleProgram_, "texelSize");
  upsampleRadius_ = glGetUniformLocation(upsampleProgram_, "radius");
  textColor_ = glGetUniformLocation(textProgram_, "textColor");
  textScale_ = glGetUniformLocation(textProgram_, "messageScale");
  textHasColor_ = glGetUniformLocation(textProgram_, "hasColor");
  centreImageExtentTo_ = glGetUniformLocation(centreImageProgram_, "halfExtentTo");
  centreImageExtentFrom_ = glGetUniformLocation(centreImageProgram_, "halfExtentFrom");
  centreImageFadeUniform_ = glGetUniformLocation(centreImageProgram_, "fade");
  centreImageHasFrom_ = glGetUniformLocation(centreImageProgram_, "hasFrom");
  glowBlurAxisTexel_ = glGetUniformLocation(glowBlurProgram_, "axisTexel");
  glowBlurSigma_ = glGetUniformLocation(glowBlurProgram_, "sigma");
  glowOverlayOpacity_ = glGetUniformLocation(glowOverlayProgram_, "opacity");
  glowOverlayOverdrive_ = glGetUniformLocation(glowOverlayProgram_, "overdrive");
  glowOverlayClamped_ = glGetUniformLocation(glowOverlayProgram_, "glowClamped");
  glowOverlayBlendMode_ = glGetUniformLocation(glowOverlayProgram_, "blendMode");
  auto encodeUniforms = [](uint32_t program) {
    EncodeUniforms uniforms;
    uniforms.outputSize = glGetUniformLocation(program, "outputSize");
    uniforms.strideUints = glGetUniformLocation(program, "strideUints");
    uniforms.limitedRange = glGetUniformLocation(program, "limitedRange");
    uniforms.exposure = glGetUniformLocation(program, "exposure");
    uniforms.flipY = glGetUniformLocation(program, "flipY");
    return uniforms;
  };
  p010Uniforms_ = encodeUniforms(p010Program_);
  nv12Uniforms_ = encodeUniforms(nv12Program_);
  if (compositeBackground_ < 0 || compositeUseFluid_ < 0 || compositeBlendMode_ < 0 ||
      p010Uniforms_.outputSize < 0 ||
      fluidUniforms_.bandResolution < 0 || fluidUniforms_.bandWidthFraction < 0 ||
      fluidUniforms_.vignetteColor < 0 || fluidUniforms_.vignetteOpacity < 0 ||
      fluidUniforms_.vignetteSize < 0 || textColor_ < 0 ||
      textScale_ < 0 || centreImageExtentTo_ < 0 || centreImageExtentFrom_ < 0 ||
      centreImageFadeUniform_ < 0 || centreImageHasFrom_ < 0 ||
      glowBlurAxisTexel_ < 0 || glowBlurSigma_ < 0 ||
      glowOverlayOpacity_ < 0 || glowOverlayOverdrive_ < 0 || glowOverlayClamped_ < 0 ||
      glowOverlayBlendMode_ < 0) {
    error = "expected uniforms were optimised out of the shader programs";
    return false;
  }

  glGenVertexArrays(1, &vao_);
  glGenTextures(1, &messageTexture_);
  glBindTexture(GL_TEXTURE_2D, messageTexture_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width_, height_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenBuffers(1, &uniformBuffer_);
  glBindBuffer(GL_UNIFORM_BUFFER, uniformBuffer_);
  glBufferData(GL_UNIFORM_BUFFER, sizeof(GpuUniforms), nullptr, GL_DYNAMIC_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  // Fonts. Failure here is never fatal: a message that cannot be rasterised is
  // dropped, which is strictly better than refusing to stream at all. The paths
  // are overridable so a developer box without the install tree still works.
  {
    const char* fontRoot = std::getenv("NOVA_VISUALISER_FONT_DIR");
    const std::string root = fontRoot != nullptr ? std::string(fontRoot)
                                                 : std::string("/opt/nova-visualiser/fonts");
    std::string fontError;
    if (!text_.load(root + "/Rajdhani-Medium.ttf", root + "/AppleColorEmoji.ttf", fontError)) {
      std::fprintf(stderr, "nova-visualiser: centre-message text disabled: %s\n",
                   fontError.c_str());
    }
  }

  glGenQueries(2, timerQueries_.data());
  // Fresh names after a GPU restart: the ping-pong must not read a query object
  // that this context has never begun.
  timerSlot_ = 0;
  timerPrimed_ = false;
  timerActive_ = false;

  if (!ensureTargets(error)) return false;

  const std::string glError = drainGlErrors("renderer init");
  if (!glError.empty()) {
    error = glError;
    return false;
  }
  return true;
}

bool Renderer::ensureTargets(std::string& error) {
  auto makeColourTarget = [](uint32_t& texture, uint32_t& fbo, int w, int h) {
    if (texture != 0) glDeleteTextures(1, &texture);
    if (fbo != 0) glDeleteFramebuffers(1, &fbo);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, std::max(1, w), std::max(1, h));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  };

  // The scene renders at the (possibly reduced) render size; the composite is
  // always full size so the encode surface geometry never changes.
  makeColourTarget(sceneTexture_, sceneFbo_, renderWidth_, renderHeight_);
  makeColourTarget(compositeTexture_, compositeFbo_, width_, height_);
  makeColourTarget(glowTexture_, glowFbo_, width_, height_);

  // The glow blur runs on a quarter-resolution copy. That is not only a cost
  // decision: the reference sigma in `core/glow_overlay_reference.h` is
  // expressed in this target's texels, and tvOS blurs at the same divisor, so
  // changing it here alone would make the two engines disagree about softness.
  glowBlurWidth_ = std::max(1, width_ / nova::kGlowBlurDownsample);
  glowBlurHeight_ = std::max(1, height_ / nova::kGlowBlurDownsample);
  for (size_t slot = 0; slot < glowBlurTextures_.size(); ++slot) {
    makeColourTarget(glowBlurTextures_[slot], glowBlurFbos_[slot], glowBlurWidth_,
                     glowBlurHeight_);
  }

  // The backdrop has the same normalized geometry as the Apple TV's
  // FluidBackgroundView (including its centred one-third-height band), but the
  // streamed renderer owns a 4K output. Rendering it at a quarter resolution
  // and magnifying it made the field noticeably soft/washed-out and let the
  // texture filter alter the apparent blob edges. Keep the authored blob size
  // in normalized band space; only give that exact field all of the output's
  // pixels.
  fluidWidth_ = std::max(1, width_);
  fluidHeight_ = std::max(1, height_);
  makeColourTarget(fluidTexture_, fluidFbo_, fluidWidth_, fluidHeight_);

  int mipWidth = std::max(1, renderWidth_ / 2);
  int mipHeight = std::max(1, renderHeight_ / 2);
  for (int index = 0; index < kBloomMips; ++index) {
    bloomWidths_[static_cast<size_t>(index)] = mipWidth;
    bloomHeights_[static_cast<size_t>(index)] = mipHeight;
    makeColourTarget(bloomTextures_[static_cast<size_t>(index)],
                     bloomFbos_[static_cast<size_t>(index)], mipWidth, mipHeight);
    mipWidth = std::max(1, mipWidth / 2);
    mipHeight = std::max(1, mipHeight / 2);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  // The targets were just reallocated, so the backdrop texture is blank. Reset
  // the divisor phase so it is redrawn on the very next frame rather than
  // leaving the composite to sample an empty texture until the phase comes back
  // around.
  const std::string glError = drainGlErrors("render targets");
  if (!glError.empty()) {
    error = glError;
    return false;
  }
  return true;
}

void Renderer::renderFluidBackground(double time) {
  glBindFramebuffer(GL_FRAMEBUFFER, fluidFbo_);
  glViewport(0, 0, fluidWidth_, fluidHeight_);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(fluidProgram_);
  // Clamped here rather than trusted from the driver: a lane is allowed to
  // overshoot its declared range by design, and a band wider than the frame
  // would put `bandLeft` outside the drawable.
  const float heightFraction = std::clamp(fluid_.heightFraction, 0.0f, 1.0f);
  const float widthFraction = std::clamp(fluid_.widthFraction, 0.0f, 1.0f);
  // The band's own drawable size, which is what sets the field aspect and the
  // grain frequency on tvOS. Both axes now, because the band is no longer
  // full-width -- using the frame width here would stretch the blob field
  // sideways as the band narrowed.
  glUniform2f(fluidUniforms_.bandResolution,
              static_cast<float>(fluidWidth_) * widthFraction,
              static_cast<float>(fluidHeight_) * heightFraction);
  glUniform1f(fluidUniforms_.bandFraction, heightFraction);
  glUniform1f(fluidUniforms_.bandWidthFraction, widthFraction);
  glUniform3f(fluidUniforms_.vignetteColor, fluid_.vignette.x, fluid_.vignette.y,
              fluid_.vignette.z);
  glUniform1f(fluidUniforms_.vignetteOpacity, std::clamp(fluid_.vignetteOpacity, 0.0f, 1.0f));
  glUniform1f(fluidUniforms_.vignetteSize, std::max(0.0f, fluid_.vignetteSize));
  glUniform1f(fluidUniforms_.time, static_cast<float>(time));
  glUniform3f(fluidUniforms_.background, fluid_.background.x, fluid_.background.y,
              fluid_.background.z);
  glUniform3f(fluidUniforms_.accent, fluid_.accent.x, fluid_.accent.y, fluid_.accent.z);
  glUniform3f(fluidUniforms_.highlight, fluid_.highlight.x, fluid_.highlight.y,
              fluid_.highlight.z);
  glUniform1f(fluidUniforms_.peakIntensity, fluid_.peakIntensity);
  glUniform1f(fluidUniforms_.falloffPower, fluid_.falloffPower);
  glUniform1f(fluidUniforms_.warpAmplitude, fluid_.warpAmplitude);
  glUniform1f(fluidUniforms_.hueSpread, fluid_.hueSpread);
  glUniform1f(fluidUniforms_.apexGlow, fluid_.apexGlow);
  glUniform1f(fluidUniforms_.blobScale, fluid_.blobScale);
  glUniform1f(fluidUniforms_.blobSoftness, fluid_.blobSoftness);

  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

void Renderer::updateMessageTexture(const std::string& message) {
  if (message == cachedMessage_) return;
  cachedMessage_ = message;

  // Layout mirrors the SwiftUI overlay this replaced: 54pt Rajdhani in a
  // 1920x1080 point space, 160pt of horizontal padding either side, at most
  // three lines. Scaled to the render target so 4K matches 1080p.
  const double scale = static_cast<double>(height_) / 1080.0;
  const int pixelSize = std::max(8, static_cast<int>(std::lround(54.0 * scale)));
  const int maxWidth = std::max(64, width_ - static_cast<int>(std::lround(320.0 * scale)));

  const RasterizedText raster =
      text_.ready() ? text_.render(message, width_, height_, pixelSize, maxWidth)
                    : RasterizedText{};
  const std::vector<uint8_t> empty(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
  const std::vector<uint8_t>& coverage = raster.coverage.empty() ? empty : raster.coverage;

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBindTexture(GL_TEXTURE_2D, messageTexture_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RED, GL_UNSIGNED_BYTE,
                  coverage.data());

  // The colour plane only exists once a message has actually contained an
  // emoji, so a text-only stream never pays for an 8 MB RGBA upload.
  messageHasColor_ = raster.hasColor;
  if (raster.hasColor) {
    if (messageColorTexture_ == 0) {
      glGenTextures(1, &messageColorTexture_);
      glBindTexture(GL_TEXTURE_2D, messageColorTexture_);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width_, height_);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, messageColorTexture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE,
                    raster.color.data());
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void Renderer::renderMessage(const SceneSnapshot& snapshot) {
  updateMessageTexture(snapshot.message);
  if (snapshot.message.empty()) return;

  glEnable(GL_BLEND);
  glUseProgram(textProgram_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, messageTexture_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, messageHasColor_ ? messageColorTexture_ : messageTexture_);
  glUniform4f(textColor_, snapshot.messageColor.x, snapshot.messageColor.y,
              snapshot.messageColor.z, snapshot.messageColor.w);
  glUniform1f(textScale_, snapshot.messageScale);
  glUniform1i(textHasColor_, messageHasColor_ ? 1 : 0);
  // Emoji arrive premultiplied; the text mask does not. One blend func cannot
  // serve both, so the shader premultiplies the tinted text instead.
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisable(GL_BLEND);
  // Leave the active unit where every other pass expects to find it.
  glActiveTexture(GL_TEXTURE0);
}

bool Renderer::bindCentreImage(int slot, const std::shared_ptr<const DecodedImage>& image) {
  glActiveTexture(GL_TEXTURE0 + static_cast<uint32_t>(slot));
  if (!image || image->width <= 0 || image->height <= 0) {
    // Still bind something: a sampler left pointing at a deleted or unwritten
    // texture is undefined, and the shader's own extent check is what actually
    // stops the plane being drawn.
    glBindTexture(GL_TEXTURE_2D, messageTexture_);
    cachedCentreImages_[static_cast<size_t>(slot)].reset();
    return false;
  }

  const size_t index = static_cast<size_t>(slot);
  if (cachedCentreImages_[index] == image) {
    glBindTexture(GL_TEXTURE_2D, centreImageTextures_[index]);
    return true;
  }

  // Reallocated per image rather than kept at a fixed size: these are sized to
  // the picture, not to the frame, and glTexStorage2D is immutable.
  if (centreImageTextures_[index] != 0) glDeleteTextures(1, &centreImageTextures_[index]);
  glGenTextures(1, &centreImageTextures_[index]);
  glBindTexture(GL_TEXTURE_2D, centreImageTextures_[index]);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, image->width, image->height);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->width, image->height, GL_RGBA,
                  GL_UNSIGNED_BYTE, image->rgba.data());
  cachedCentreImages_[index] = image;
  return true;
}

// The other half of the centre slot. Drawn immediately before the message and
// therefore before the glow overlay, so an image blooms with the rest of the
// picture exactly as the text does.
void Renderer::renderCentreImage(const SceneSnapshot& snapshot) {
  const bool hasTo = bindCentreImage(0, snapshot.centreImage);
  const bool hasFrom = bindCentreImage(1, snapshot.centreImageFrom);
  if (!hasTo && !hasFrom) {
    glActiveTexture(GL_TEXTURE0);
    return;
  }

  // Contain-fit and scale come from the shared reference header rather than
  // being worked out in the shader, so the conformance corpus can lock them.
  const float frameAspect = renderHeight_ > 0
      ? static_cast<float>(renderWidth_) / static_cast<float>(renderHeight_)
      : 0.0f;
  auto extentOf = [&](const std::shared_ptr<const DecodedImage>& image) {
    if (!image || image->height <= 0) return CentreImageExtent{};
    const float aspect = static_cast<float>(image->width) / static_cast<float>(image->height);
    return centreImageHalfExtent(frameAspect, aspect, snapshot.centreImageHeight,
                                 snapshot.messageScale);
  };
  const CentreImageExtent to = extentOf(snapshot.centreImage);
  const CentreImageExtent from = extentOf(snapshot.centreImageFrom);

  glEnable(GL_BLEND);
  glUseProgram(centreImageProgram_);
  glUniform2f(centreImageExtentTo_, to.halfWidth, to.halfHeight);
  glUniform2f(centreImageExtentFrom_, from.halfWidth, from.halfHeight);
  glUniform1f(centreImageFadeUniform_, snapshot.centreImageFade);
  glUniform1i(centreImageHasFrom_, hasFrom ? 1 : 0);
  // Both planes are premultiplied at decode time.
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDisable(GL_BLEND);
  // Leave the active unit where every other pass expects to find it.
  glActiveTexture(GL_TEXTURE0);
}

void Renderer::setResolutionScale(float scale) {
  const float clamped = std::min(1.0f, std::max(0.5f, scale));
  const int nextWidth = std::max(2, static_cast<int>(width_ * clamped) & ~1);
  const int nextHeight = std::max(2, static_cast<int>(height_ * clamped) & ~1);
  if (nextWidth == renderWidth_ && nextHeight == renderHeight_) {
    resolutionScale_ = clamped;
    return;
  }
  resolutionScale_ = clamped;
  renderWidth_ = nextWidth;
  renderHeight_ = nextHeight;
  std::string error;
  ensureTargets(error);
}

bool Renderer::ensureParticleCapacity(size_t bytes, std::string& error) {
  auto grow = [&](ParticleRing& ring) {
    if (ring.capacityBytes >= bytes && ring.buffers[0] != 0) return;
    const size_t capacity = std::max<size_t>(bytes * 2, 1u << 20);
    for (int index = 0; index < ParticleRing::kFrames; ++index) {
      if (ring.fences[static_cast<size_t>(index)] != nullptr) {
        glDeleteSync(static_cast<GLsync>(ring.fences[static_cast<size_t>(index)]));
        ring.fences[static_cast<size_t>(index)] = nullptr;
      }
      if (ring.buffers[static_cast<size_t>(index)] != 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring.buffers[static_cast<size_t>(index)]);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        glDeleteBuffers(1, &ring.buffers[static_cast<size_t>(index)]);
      }
      glGenBuffers(1, &ring.buffers[static_cast<size_t>(index)]);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring.buffers[static_cast<size_t>(index)]);
      // Persistent + coherent mapping: the CPU writes straight into GPU-visible
      // memory with no driver-side staging copy and no per-frame map/unmap.
      const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
      glBufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(capacity), nullptr, flags);
      ring.mapped[static_cast<size_t>(index)] =
          glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(capacity), flags);
    }
    ring.capacityBytes = capacity;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  };

  grow(current_);
  grow(previous_);
  if (current_.mapped[0] == nullptr || previous_.mapped[0] == nullptr) {
    error = "could not persistently map the particle ring buffers";
    return false;
  }
  return true;
}

void Renderer::uploadParticles(const std::vector<RenderParticle>& particles, int slot) {
  ParticleRing& ring = slot == 0 ? current_ : previous_;
  const int index = ring.cursor;
  // Wait for the GPU to be finished with this ring slot from two frames ago.
  if (ring.fences[static_cast<size_t>(index)] != nullptr) {
    glClientWaitSync(static_cast<GLsync>(ring.fences[static_cast<size_t>(index)]),
                     GL_SYNC_FLUSH_COMMANDS_BIT, 1'000'000'000);
    glDeleteSync(static_cast<GLsync>(ring.fences[static_cast<size_t>(index)]));
    ring.fences[static_cast<size_t>(index)] = nullptr;
  }
  if (!particles.empty()) {
    std::memcpy(ring.mapped[static_cast<size_t>(index)], particles.data(),
                particles.size() * sizeof(RenderParticle));
  }
}

bool Renderer::render(const SnapshotPtr& latest, const SnapshotPtr& previous, float alpha,
                      float /*exposure*/, std::string& error) {
  if (!latest) return true;

  if (!timerActive_) {
    glBeginQuery(GL_TIME_ELAPSED, timerQueries_[timerSlot_]);
    timerActive_ = true;
  }

  const std::vector<RenderParticle>& particles = latest->particles;
  const size_t bytes = std::max<size_t>(particles.size() * sizeof(RenderParticle), 1);
  if (!ensureParticleCapacity(bytes, error)) return false;

  // Interpolation is only valid between two snapshots of the same scene: a
  // structural rebuild renumbers everything, so it snaps instead.
  const bool canInterpolate = previous && previous->structureSerial == latest->structureSerial &&
                              previous->particles.size() == particles.size() &&
                              !particles.empty();

  uploadParticles(particles, 0);
  if (canInterpolate) uploadParticles(previous->particles, 1);

  GpuUniforms uniforms{};
  uniforms.viewport[0] = static_cast<float>(renderWidth_);
  uniforms.viewport[1] = static_cast<float>(renderHeight_);
  uniforms.viewport[2] = resolutionScale_;
  uniforms.viewport[3] = std::max(1.0f, static_cast<float>(height_) / 1080.0f);
  uniforms.boundsMin[0] = latest->boundsMinimum.x;
  uniforms.boundsMin[1] = latest->boundsMinimum.y;
  uniforms.boundsMin[2] = latest->boundsMinimum.z;
  uniforms.boundsMax[0] = latest->boundsMaximum.x;
  uniforms.boundsMax[1] = latest->boundsMaximum.y;
  uniforms.boundsMax[2] = latest->boundsMaximum.z;
  uniforms.signalData[0] = static_cast<float>(latest->signal.time);
  uniforms.signalData[1] = static_cast<float>(latest->signal.beatPulse);
  uniforms.signalData[2] = latest->is3D ? 1.0f : 0.0f;
  uniforms.signalData[3] = static_cast<float>(static_cast<int>(latest->signal.quality));
  uniforms.blend[0] = alpha;
  uniforms.blend[1] = canInterpolate ? 1.0f : 0.0f;

  glBindBuffer(GL_UNIFORM_BUFFER, uniformBuffer_);
  glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GpuUniforms), &uniforms);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  // --- scene pass -----------------------------------------------------------
  glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
  glViewport(0, 0, renderWidth_, renderHeight_);
  // Cleared transparent so the background colour cannot leak into the bright
  // pass; the composite adds it afterwards, exactly as tvOS does.
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  if (!particles.empty()) {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(particleProgram_);
    glBindVertexArray(vao_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kCurrentBinding,
                     current_.buffers[static_cast<size_t>(current_.cursor)]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kPreviousBinding,
                     canInterpolate ? previous_.buffers[static_cast<size_t>(previous_.cursor)]
                                    : current_.buffers[static_cast<size_t>(current_.cursor)]);
    glBindBufferBase(GL_UNIFORM_BUFFER, kUniformBinding, uniformBuffer_);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(particles.size()));
    glDisable(GL_BLEND);
  }

  runBloom();

  // --- backdrop -------------------------------------------------------------
  // Redrawn at the output frame rate. This surface is intentionally no longer
  // configurable or capped independently of the visualiser.
  if (fluid_.enabled) {
    // Integrate `dt * speed` rather than passing `elapsed * speed`. `speed`
    // comes from the chased `fluid_speed` setting and therefore changes
    // constantly; scaling a running clock by it makes the phase jump by
    // `elapsed * delta_speed` every time it moves, which reads as the backdrop
    // jolting. Accumulating keeps it continuous, and speed 0 holds the phase.
    // Mirrors `FluidBackgroundRenderer.advancePhase` on tvOS.
    const double nowSeconds = backdropSeconds();
    if (fluidLastSeconds_ >= 0) {
      const double delta = std::min(0.25, std::max(0.0, nowSeconds - fluidLastSeconds_));
      fluidPhase_ += delta * static_cast<double>(fluid_.speed);
    }
    fluidLastSeconds_ = nowSeconds;
    renderFluidBackground(fluidPhase_);
  }
  // --- composite ------------------------------------------------------------
  glBindFramebuffer(GL_FRAMEBUFFER, compositeFbo_);
  glViewport(0, 0, width_, height_);
  glUseProgram(compositeProgram_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sceneTexture_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, bloomTextures_[0]);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, fluidTexture_);
  glUniform1f(compositeIntensity_, kCompositeIntensity);
  glUniform1i(compositeUseFluid_, fluid_.enabled ? 1 : 0);
  glUniform4f(compositeBackground_, latest->background.x, latest->background.y,
              latest->background.z, latest->background.w);
  glUniform1i(compositeBlendMode_, static_cast<int>(latest->sceneBlendMode));
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // --- centre slot ----------------------------------------------------------
  // Drawn into the composite target, on top of everything and outside the bloom
  // chain so the centrepiece stays legible rather than blooming into a smear.
  // The message call was once missing entirely: `renderMessage` existed but
  // nothing invoked it, so the message was never in the stream at any point,
  // which is why it had to be drawn client-side on tvOS.
  //
  // Only one of these two ever draws anything in a given frame -- the
  // simulation decides which -- except during a colour-theme change, when the
  // image pass dissolves between two images.
  renderCentreImage(*latest);
  renderMessage(*latest);

  // --- glow overlay ---------------------------------------------------------
  // Deliberately after the message: this is the last thing that happens to the
  // picture, and the text is part of the picture.
  runGlowOverlay(*latest);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Fence the ring slots so the next pass through knows when they are free.
  current_.fences[static_cast<size_t>(current_.cursor)] =
      glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  current_.cursor = (current_.cursor + 1) % ParticleRing::kFrames;
  if (canInterpolate) {
    previous_.fences[static_cast<size_t>(previous_.cursor)] =
        glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    previous_.cursor = (previous_.cursor + 1) % ParticleRing::kFrames;
  }

  stats_.particleCount = static_cast<int>(particles.size());
  stats_.renderWidth = renderWidth_;
  stats_.renderHeight = renderHeight_;
  stats_.resolutionScale = resolutionScale_;
  return true;
}

void Renderer::runGlowOverlay(const SceneSnapshot& snapshot) {
  const float opacity = std::max(0.0f, std::min(1.0f, snapshot.glowOpacity / 100.0f));
  // Fully transparent is the default, and it is exactly the identity. Skipping
  // costs nothing when nobody has turned the layer on, which matters on a GPU
  // shared with the voice stack.
  if (opacity <= 0.0005f) return;

  const float sigma = glowBlurSigmaTexels(snapshot.glowBlurAmount, static_cast<float>(height_));

  glBindVertexArray(vao_);
  glUseProgram(glowBlurProgram_);
  glViewport(0, 0, glowBlurWidth_, glowBlurHeight_);
  glActiveTexture(GL_TEXTURE0);

  // Horizontal: full-resolution frame down into the quarter-resolution target.
  glBindFramebuffer(GL_FRAMEBUFFER, glowBlurFbos_[0]);
  glBindTexture(GL_TEXTURE_2D, compositeTexture_);
  glUniform2f(glowBlurAxisTexel_, 1.0f / static_cast<float>(glowBlurWidth_), 0.0f);
  glUniform1f(glowBlurSigma_, sigma);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // Vertical.
  glBindFramebuffer(GL_FRAMEBUFFER, glowBlurFbos_[1]);
  glBindTexture(GL_TEXTURE_2D, glowBlurTextures_[0]);
  glUniform2f(glowBlurAxisTexel_, 0.0f, 1.0f / static_cast<float>(glowBlurHeight_));
  glUniform1f(glowBlurSigma_, sigma);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // Blend back over the frame. A fragment shader rather than fixed-function
  // blending, so this side evaluates the same expression as
  // `glowOverlayReference()` and `phonoscope_glow_overlay` instead of a
  // per-mode pair of blend factors that only happens to agree with them.
  glBindFramebuffer(GL_FRAMEBUFFER, glowFbo_);
  glViewport(0, 0, width_, height_);
  glUseProgram(glowOverlayProgram_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, compositeTexture_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, glowBlurTextures_[1]);
  glUniform1f(glowOverlayOpacity_, opacity);
  glUniform1f(glowOverlayOverdrive_, snapshot.glowOverdrive);
  glUniform1i(glowOverlayClamped_, snapshot.glowClamped ? 1 : 0);
  glUniform1i(glowOverlayBlendMode_, static_cast<int>(snapshot.glowBlendMode));
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // The result is now the finished frame, so it becomes the composite. Encode,
  // readback and diagnostics all keep reading `compositeTexture_`/`compositeFbo_`
  // and need to know nothing about this pass.
  std::swap(compositeTexture_, glowTexture_);
  std::swap(compositeFbo_, glowFbo_);

  // Leave the active unit where every other pass expects to find it.
  glActiveTexture(GL_TEXTURE0);
}

void Renderer::runBloom() {
  glBindVertexArray(vao_);
  glActiveTexture(GL_TEXTURE0);

  // Bright pass into mip 0.
  glBindFramebuffer(GL_FRAMEBUFFER, bloomFbos_[0]);
  glViewport(0, 0, bloomWidths_[0], bloomHeights_[0]);
  glUseProgram(extractProgram_);
  glBindTexture(GL_TEXTURE_2D, sceneTexture_);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // Progressive downsample.
  glUseProgram(downsampleProgram_);
  for (int index = 1; index < kBloomMips; ++index) {
    const size_t source = static_cast<size_t>(index - 1);
    const size_t destination = static_cast<size_t>(index);
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFbos_[destination]);
    glViewport(0, 0, bloomWidths_[destination], bloomHeights_[destination]);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[source]);
    glUniform2f(downsampleTexel_, 1.0f / static_cast<float>(bloomWidths_[source]),
                1.0f / static_cast<float>(bloomHeights_[source]));
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  // Energy-preserving upsample back to mip 0: mix(destination, upsampled,
  // kBloomScatter) rather than destination + upsampled. See kBloomScatter.
  glEnable(GL_BLEND);
  glBlendColor(kBloomScatter, kBloomScatter, kBloomScatter, kBloomScatter);
  glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR);
  glUseProgram(upsampleProgram_);
  for (int index = kBloomMips - 1; index > 0; --index) {
    const size_t source = static_cast<size_t>(index);
    const size_t destination = static_cast<size_t>(index - 1);
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFbos_[destination]);
    glViewport(0, 0, bloomWidths_[destination], bloomHeights_[destination]);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[source]);
    glUniform2f(upsampleTexel_, 1.0f / static_cast<float>(bloomWidths_[source]),
                1.0f / static_cast<float>(bloomHeights_[source]));
    glUniform1f(upsampleRadius_, visualEffectScale(static_cast<float>(height_)));
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  glDisable(GL_BLEND);
}

int Renderer::addOutput(int width, int height, PixelLayout layout, int lumaStrideBytes,
                        int chromaStrideBytes, std::string& error) {
  OutputPlane plane;
  plane.width = width;
  plane.height = height;
  plane.layout = layout;
  plane.lumaStrideBytes = lumaStrideBytes;
  plane.chromaStrideBytes = chromaStrideBytes;

  const size_t lumaBytes = static_cast<size_t>(lumaStrideBytes) * static_cast<size_t>(height);
  const size_t chromaBytes =
      static_cast<size_t>(chromaStrideBytes) * static_cast<size_t>((height + 1) / 2);

  glGenBuffers(1, &plane.lumaBuffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, plane.lumaBuffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(lumaBytes), nullptr,
               GL_DYNAMIC_COPY);
  glGenBuffers(1, &plane.chromaBuffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, plane.chromaBuffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(chromaBytes), nullptr,
               GL_DYNAMIC_COPY);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

  const std::string glError = drainGlErrors("output plane allocation");
  if (!glError.empty()) {
    error = glError;
    return -1;
  }
  outputs_.push_back(plane);
  return static_cast<int>(outputs_.size()) - 1;
}

void Renderer::convertOutputs(float exposure) {
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, compositeTexture_);

  for (const OutputPlane& plane : outputs_) {
    const bool isP010 = plane.layout == PixelLayout::P010;
    const EncodeUniforms& uniforms = isP010 ? p010Uniforms_ : nv12Uniforms_;
    glUseProgram(isP010 ? p010Program_ : nv12Program_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, plane.lumaBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, plane.chromaBuffer);
    glUniform2i(uniforms.outputSize, plane.width, plane.height);
    glUniform2i(uniforms.strideUints, plane.lumaStrideBytes / 4, plane.chromaStrideBytes / 4);
    glUniform1i(uniforms.limitedRange, plane.limitedRange ? 1 : 0);
    glUniform1f(uniforms.exposure, exposure);
    glUniform1i(uniforms.flipY, 1);

    // P010 covers a 2x2 pixel tile per invocation, NV12 a 4x2 tile.
    const int tileWidth = isP010 ? 2 : 4;
    const int groupsX = (plane.width + tileWidth * 16 - 1) / (tileWidth * 16);
    const int groupsY = (plane.height + 2 * 16 - 1) / (2 * 16);
    glDispatchCompute(static_cast<GLuint>(groupsX), static_cast<GLuint>(groupsY), 1);
  }
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

  if (timerActive_) {
    glEndQuery(GL_TIME_ELAPSED);
    timerActive_ = false;

    // Read the PREVIOUS frame's query, not the one that was just ended. A
    // GL_TIME_ELAPSED result is essentially never available in the same call
    // that closed it -- the GPU has not finished the work yet -- so polling it
    // immediately meant the availability check failed every single frame and
    // this stat read a constant zero. Ping-ponging between two query objects
    // gives the GPU a whole frame to retire the first one, which is what the
    // second query object was allocated for in the first place.
    const size_t other = (timerSlot_ + 1) % timerQueries_.size();
    // `other` holds the query the PREVIOUS frame ended. On the very first frame
    // it has never been begun at all, and querying an unused name is an
    // INVALID_OPERATION rather than a miss -- hence the primed flag.
    if (timerPrimed_) {
      GLuint64 elapsed = 0;
      GLint available = 0;
      glGetQueryObjectiv(timerQueries_[other], GL_QUERY_RESULT_AVAILABLE, &available);
      if (available != 0) {
        glGetQueryObjectui64v(timerQueries_[other], GL_QUERY_RESULT, &elapsed);
        stats_.gpuMilliseconds = static_cast<double>(elapsed) / 1e6;
      }
    } else {
      timerPrimed_ = true;
    }
    timerSlot_ = other;
  }
}

void Renderer::finish() { glFinish(); }

std::string Renderer::stageSummary() {
  // Reads the brightest of a coarse grid of texels from each stage, so a pass
  // that produced something faint is not mistaken for one that produced nothing.
  auto peek = [](uint32_t fbo, int width, int height) {
    const int step = std::max(1, width / 64);
    std::vector<float> row(static_cast<size_t>(width) * 4);
    float brightestColour = 0;
    float brightestAlpha = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    for (int y = 0; y < height; y += std::max(1, height / 32)) {
      glReadPixels(0, y, width, 1, GL_RGBA, GL_FLOAT, row.data());
      for (int x = 0; x < width; x += step) {
        const size_t base = static_cast<size_t>(x) * 4;
        for (int channel = 0; channel < 3; ++channel) {
          brightestColour = std::max(brightestColour, row[base + static_cast<size_t>(channel)]);
        }
        brightestAlpha = std::max(brightestAlpha, row[base + 3]);
      }
    }
    return std::pair<float, float>{brightestColour, brightestAlpha};
  };

  std::string summary;
  char buffer[288];
  const std::pair<float, float> scene = peek(sceneFbo_, renderWidth_, renderHeight_);
  const std::pair<float, float> bloom = peek(bloomFbos_[0], bloomWidths_[0], bloomHeights_[0]);
  const std::pair<float, float> backdrop =
      fluid_.enabled ? peek(fluidFbo_, fluidWidth_, fluidHeight_) : std::pair<float, float>{0, 0};
  const std::pair<float, float> composite = peek(compositeFbo_, width_, height_);
  // Colour and alpha are reported separately: a stage that is bright only in
  // alpha looks fine on a combined maximum while producing a black image.
  //
  // `bloom0 rgb` is the number that mattered most here. The upsample chain used
  // to accumulate additively, so it read far above the bright pass and washed
  // the whole composite pale; it should now sit close to the scene's own peak.
  std::snprintf(buffer, sizeof(buffer),
                "scene rgb=%.4f a=%.4f | bloom0 rgb=%.4f a=%.4f | backdrop rgb=%.4f a=%.4f | "
                "composite rgb=%.4f a=%.4f",
                scene.first, scene.second, bloom.first, bloom.second, backdrop.first,
                backdrop.second, composite.first, composite.second);
  summary = buffer;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  const std::string glError = drainGlErrors("stage summary");
  if (!glError.empty()) summary += " [" + glError + "]";
  return summary;
}

bool Renderer::readbackRgba(std::vector<uint8_t>& out, int& width, int& height, float exposure) {
  width = width_;
  height = height_;
  const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height);

  // The composite target is RGBA16F and holds genuinely-HDR values. GL_FLOAT is
  // the only read type guaranteed to work against a float attachment -- asking
  // for GL_UNSIGNED_BYTE returns zeros on this driver rather than converting.
  std::vector<float> linear(texels * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, compositeFbo_);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, linear.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  if (!drainGlErrors("readback").empty()) return false;

  // Match the stream and the native Metal composite target: colours are
  // display-referred, and the final output is an 8-bit UNORM clamp. This keeps
  // a frame dump faithful to the picked theme colour instead of previewing a
  // second tone-mapped version of it.
  out.resize(texels * 4);
  for (size_t index = 0; index < texels * 4; index += 4) {
    for (int channel = 0; channel < 3; ++channel) {
      const float value = std::max(0.0f, linear[index + static_cast<size_t>(channel)]) * exposure;
      out[index + static_cast<size_t>(channel)] =
          static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, value)) * 255.0f));
    }
    out[index + 3] = 255;
  }
  return true;
}

void Renderer::shutdown() {
  auto releaseRing = [](ParticleRing& ring) {
    for (int index = 0; index < ParticleRing::kFrames; ++index) {
      const size_t slot = static_cast<size_t>(index);
      if (ring.fences[slot] != nullptr) {
        glDeleteSync(static_cast<GLsync>(ring.fences[slot]));
        ring.fences[slot] = nullptr;
      }
      if (ring.buffers[slot] != 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring.buffers[slot]);
        if (ring.mapped[slot] != nullptr) glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        glDeleteBuffers(1, &ring.buffers[slot]);
        ring.buffers[slot] = 0;
      }
      ring.mapped[slot] = nullptr;
    }
    ring.capacityBytes = 0;
  };
  releaseRing(current_);
  releaseRing(previous_);

  for (OutputPlane& plane : outputs_) {
    if (plane.lumaBuffer != 0) glDeleteBuffers(1, &plane.lumaBuffer);
    if (plane.chromaBuffer != 0) glDeleteBuffers(1, &plane.chromaBuffer);
  }
  outputs_.clear();

  for (int index = 0; index < kBloomMips; ++index) {
    const size_t slot = static_cast<size_t>(index);
    if (bloomTextures_[slot] != 0) glDeleteTextures(1, &bloomTextures_[slot]);
    if (bloomFbos_[slot] != 0) glDeleteFramebuffers(1, &bloomFbos_[slot]);
    bloomTextures_[slot] = 0;
    bloomFbos_[slot] = 0;
  }
  if (sceneTexture_ != 0) glDeleteTextures(1, &sceneTexture_);
  if (compositeTexture_ != 0) glDeleteTextures(1, &compositeTexture_);
  if (fluidTexture_ != 0) glDeleteTextures(1, &fluidTexture_);
  if (messageTexture_ != 0) glDeleteTextures(1, &messageTexture_);
  if (messageColorTexture_ != 0) glDeleteTextures(1, &messageColorTexture_);
  messageColorTexture_ = 0;
  messageHasColor_ = false;
  cachedMessage_.clear();
  for (size_t slot = 0; slot < centreImageTextures_.size(); ++slot) {
    if (centreImageTextures_[slot] != 0) glDeleteTextures(1, &centreImageTextures_[slot]);
    centreImageTextures_[slot] = 0;
    // Cleared too: the cached pointer is what says "this texture already holds
    // that image", and the texture it referred to has just gone.
    cachedCentreImages_[slot].reset();
  }
  if (glowTexture_ != 0) glDeleteTextures(1, &glowTexture_);
  for (size_t slot = 0; slot < glowBlurTextures_.size(); ++slot) {
    if (glowBlurTextures_[slot] != 0) glDeleteTextures(1, &glowBlurTextures_[slot]);
    if (glowBlurFbos_[slot] != 0) glDeleteFramebuffers(1, &glowBlurFbos_[slot]);
    glowBlurTextures_[slot] = 0;
    glowBlurFbos_[slot] = 0;
  }
  if (sceneFbo_ != 0) glDeleteFramebuffers(1, &sceneFbo_);
  if (compositeFbo_ != 0) glDeleteFramebuffers(1, &compositeFbo_);
  if (glowFbo_ != 0) glDeleteFramebuffers(1, &glowFbo_);
  if (fluidFbo_ != 0) glDeleteFramebuffers(1, &fluidFbo_);
  sceneTexture_ = compositeTexture_ = glowTexture_ = fluidTexture_ = messageTexture_ = 0;
  cachedMessage_.clear();
  sceneFbo_ = compositeFbo_ = glowFbo_ = fluidFbo_ = 0;

  for (uint32_t program : {particleProgram_, extractProgram_, downsampleProgram_, upsampleProgram_,
                           compositeProgram_, fluidProgram_, textProgram_, glowBlurProgram_,
                           glowOverlayProgram_, p010Program_, nv12Program_}) {
    if (program != 0) glDeleteProgram(program);
  }
  particleProgram_ = extractProgram_ = downsampleProgram_ = upsampleProgram_ = compositeProgram_ =
      fluidProgram_ = textProgram_ = glowBlurProgram_ = glowOverlayProgram_ = p010Program_ =
          nv12Program_ = 0;

  if (timerQueries_[0] != 0) glDeleteQueries(2, timerQueries_.data());
  timerQueries_ = {};
  timerSlot_ = 0;
  timerPrimed_ = false;
  timerActive_ = false;

  if (uniformBuffer_ != 0) glDeleteBuffers(1, &uniformBuffer_);
  if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
  uniformBuffer_ = vao_ = 0;
}

}  // namespace nova::gfx
