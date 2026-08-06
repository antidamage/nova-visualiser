// The render thread's GPU state.
//
// Owns the sole GL context. It consumes scene snapshots, never blocks on the
// simulation, and hands finished frames to the encoder as GPU-resident YUV
// planes. Particle state is uploaded through a triple-buffered persistently
// mapped ring so a frame never stalls waiting for the previous one's fence --
// which is the main structural improvement over the tvOS renderer, where every
// particle was rebuilt and memcpy'd on the draw callback.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/snapshot.h"
#include "gfx/text.h"

namespace nova::gfx {

enum class PixelLayout { P010, NV12 };

// One encode target. The renderer fills the planes of whatever rungs are
// registered; each maps to an NVENC session downstream.
struct OutputPlane {
  int width = 0;
  int height = 0;
  PixelLayout layout = PixelLayout::P010;
  // GL buffer objects holding the raw planes, registered with CUDA by the
  // encoder. Row strides are in bytes and must match the CUDA frame linesizes.
  uint32_t lumaBuffer = 0;
  uint32_t chromaBuffer = 0;
  int lumaStrideBytes = 0;
  int chromaStrideBytes = 0;
  bool limitedRange = true;
};

struct RenderStats {
  double gpuMilliseconds = 0;
  int particleCount = 0;
  int renderWidth = 0;
  int renderHeight = 0;
  float resolutionScale = 1.0f;
};

class Renderer {
 public:
  Renderer() = default;
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool initialise(int width, int height, std::string& error);
  void shutdown();

  // Registers an output rung. `layout` decides which encode pass runs and the
  // plane geometry; the returned index is used by `planeFor`.
  int addOutput(int width, int height, PixelLayout layout, int lumaStrideBytes,
                int chromaStrideBytes, std::string& error);
  const OutputPlane& planeFor(int index) const { return outputs_[static_cast<size_t>(index)]; }

  // Animated backdrop settings, mirroring tvOS `FluidBackgroundSettings` after
  // its /100 scaling. `enabled` is driven by the module manifest, not the module
  // id: a module opts in by declaring a setting that affects
  // `renderer.fluidBackground.speed`.
  struct FluidBackground {
    bool enabled = false;
    float speed = 0.65f;
    float peakIntensity = 1.0f;
    float falloffPower = 1.6f;
    float warpAmplitude = 1.0f;
    float hueSpread = 0.5f;
    float apexGlow = 1.0f;
    // tvOS passes these two for the phonoscope surface specifically.
    float blobScale = 4.0f;
    float blobSoftness = 0.45f;
    // Frame geometry, as fractions of the render view. The defaults are the
    // original fixed letterbox: a centred band one third high and full width.
    // Both are driven parameters, so they arrive resolved for this frame.
    float heightFraction = 1.0f / 3.0f;
    float widthFraction = 1.0f;
    // Vignette. The colour is a palette slot; the other two are driven. The
    // defaults reproduce the authored `PhonoscopeEdgeVignette` exactly.
    float vignetteOpacity = 0.96f;
    float vignetteSize = 1.0f;
    Vec4 background{0, 0, 0, 1};
    Vec4 accent{0, 0, 0, 1};
    Vec4 highlight{0, 0, 0, 1};
    Vec4 vignette{0, 0, 0, 1};
  };
  void setFluidBackground(const FluidBackground& settings) { fluid_ = settings; }

  // Draws one frame. `alpha` interpolates between `previous` and `latest`.
  // Returns false only on a GL error worth failing the service for.
  bool render(const SnapshotPtr& latest, const SnapshotPtr& previous, float alpha,
              float exposure, std::string& error);

  // Runs the encode conversion passes into the registered output planes, then
  // issues the memory barrier the CUDA mapping needs.
  void convertOutputs(float exposure);

  // Blocks until every command issued so far has completed. The encode thread
  // calls this before mapping the planes into CUDA.
  void finish();

  // Dynamic resolution. The scene renders at `scale` and is upscaled into the
  // fixed-size encode surface, so the stream resolution never changes even when
  // the GPU is busy with voice inference.
  void setResolutionScale(float scale);
  float resolutionScale() const { return resolutionScale_; }

  const RenderStats& stats() const { return stats_; }

  // Reads the composited image back as 8-bit RGBA. Only used by the offline
  // frame-dump mode that feeds the conformance corpus; never on the hot path.
  bool readbackRgba(std::vector<uint8_t>& out, int& width, int& height, float exposure);

  // Peeks one texel out of every stage. Diagnostic only: used by --dump-frame
  // to say which pass went dark rather than leaving a black frame unexplained.
  std::string stageSummary();

 private:
  struct ParticleRing {
    static constexpr int kFrames = 3;
    std::array<uint32_t, kFrames> buffers{};
    std::array<void*, kFrames> mapped{};
    std::array<void*, kFrames> fences{};
    size_t capacityBytes = 0;
    int cursor = 0;
  };

  bool ensureTargets(std::string& error);
  bool ensureParticleCapacity(size_t bytes, std::string& error);
  void uploadParticles(const std::vector<RenderParticle>& particles, int slot);
  void runBloom();
  void runGlowOverlay(const SceneSnapshot& snapshot);
  void renderFluidBackground(double time);
  void updateMessageTexture(const std::string& message);
  void renderMessage(const SceneSnapshot& snapshot);
  // Uploads `image` into `slot` if it is not already there, and reports whether
  // there is anything to draw. Identity is the shared_ptr, never the pixels.
  bool bindCentreImage(int slot, const std::shared_ptr<const DecodedImage>& image);
  void renderCentreImage(const SceneSnapshot& snapshot);

  int width_ = 0;
  int height_ = 0;
  int renderWidth_ = 0;
  int renderHeight_ = 0;
  float resolutionScale_ = 1.0f;

  uint32_t vao_ = 0;
  uint32_t particleProgram_ = 0;
  uint32_t extractProgram_ = 0;
  uint32_t downsampleProgram_ = 0;
  uint32_t upsampleProgram_ = 0;
  uint32_t compositeProgram_ = 0;
  uint32_t fluidProgram_ = 0;
  uint32_t textProgram_ = 0;
  uint32_t centreImageProgram_ = 0;
  uint32_t glowBlurProgram_ = 0;
  uint32_t glowOverlayProgram_ = 0;
  uint32_t p010Program_ = 0;
  uint32_t nv12Program_ = 0;
  uint32_t uniformBuffer_ = 0;

  // Uniform locations are resolved by name rather than declared with explicit
  // layout locations. Explicit locations sharing a program with `layout(binding)`
  // samplers is a reliable source of silently-wrong writes, and this pipeline
  // has few enough uniforms that caching them costs nothing.
  struct EncodeUniforms {
    int outputSize = -1;
    int strideUints = -1;
    int limitedRange = -1;
    int exposure = -1;
    int flipY = -1;
  };
  int compositeIntensity_ = -1;
  int compositeBackground_ = -1;
  int compositeUseFluid_ = -1;
  int compositeBlendMode_ = -1;
  int downsampleTexel_ = -1;
  int upsampleTexel_ = -1;
  int upsampleRadius_ = -1;
  int textColor_ = -1;
  int textScale_ = -1;
  int textHasColor_ = -1;
  int centreImageExtentTo_ = -1;
  int centreImageExtentFrom_ = -1;
  int centreImageFadeUniform_ = -1;
  int centreImageHasFrom_ = -1;
  int glowBlurAxisTexel_ = -1;
  int glowBlurSigma_ = -1;
  int glowOverlayOpacity_ = -1;
  int glowOverlayOverdrive_ = -1;
  int glowOverlayClamped_ = -1;
  int glowOverlayBlendMode_ = -1;
  struct FluidUniforms {
    int bandResolution = -1;
    int bandFraction = -1;
    int bandWidthFraction = -1;
    int vignetteColor = -1;
    int vignetteOpacity = -1;
    int vignetteSize = -1;
    int time = -1;

    int background = -1;
    int accent = -1;
    int highlight = -1;
    int peakIntensity = -1;
    int falloffPower = -1;
    int warpAmplitude = -1;
    int hueSpread = -1;
    int apexGlow = -1;
    int blobScale = -1;
    int blobSoftness = -1;
  };
  FluidUniforms fluidUniforms_;
  EncodeUniforms p010Uniforms_;
  EncodeUniforms nv12Uniforms_;

  uint32_t sceneFbo_ = 0;
  uint32_t sceneTexture_ = 0;
  uint32_t compositeFbo_ = 0;
  uint32_t compositeTexture_ = 0;
  // Second full-resolution target for the final glow overlay. The pass reads
  // the finished frame and writes the blended result, so it cannot write back
  // into the texture it is sampling; afterwards the two handles are swapped and
  // everything downstream keeps reading `compositeTexture_` as before.
  uint32_t glowFbo_ = 0;
  uint32_t glowTexture_ = 0;
  // Quarter-resolution ping-pong for the separable blur.
  std::array<uint32_t, 2> glowBlurFbos_{};
  std::array<uint32_t, 2> glowBlurTextures_{};
  int glowBlurWidth_ = 0;
  int glowBlurHeight_ = 0;
  // Quarter-resolution backdrop. tvOS runs the same shader at renderScale 0.25
  // and a fixed 30 fps; matching both keeps the cost negligible on a GPU that is
  // shared with the voice stack.
  uint32_t fluidFbo_ = 0;
  uint32_t fluidTexture_ = 0;
  int fluidWidth_ = 0;
  int fluidHeight_ = 0;
  FluidBackground fluid_;
  // Accumulated backdrop phase, so a changing `fluid_.speed` cannot discontinue
  // the animation. See the integration step in `render`.
  double fluidPhase_ = 0.0;
 public:
  // Diagnostic: the actual integrated backdrop phase. A driven `fluid_speed`
  // is otherwise unfalsifiable from outside -- you can see the setting move
  // without knowing whether the picture followed it.
  double fluidPhase() const { return fluidPhase_; }
 private:
  double fluidLastSeconds_ = -1.0;
  uint32_t messageTexture_ = 0;
  // Allocated lazily the first time a message actually contains an emoji.
  uint32_t messageColorTexture_ = 0;
  bool messageHasColor_ = false;
  TextRasterizer text_;
  std::string cachedMessage_;

  // The centre image's two planes: [0] is the incoming image, [1] the one still
  // fading out behind it. Allocated lazily and at the image's OWN size -- unlike
  // the message textures, which are full-frame only because glyph rasterisation
  // is. The cached pointers are what make "same image" a pointer comparison
  // instead of a hash of several megabytes every frame.
  static constexpr int kCentreImagePlanes = 2;
  std::array<uint32_t, kCentreImagePlanes> centreImageTextures_{};
  std::array<std::shared_ptr<const DecodedImage>, kCentreImagePlanes> cachedCentreImages_{};

  static constexpr int kBloomMips = 5;
  std::array<uint32_t, kBloomMips> bloomTextures_{};
  std::array<uint32_t, kBloomMips> bloomFbos_{};
  std::array<int, kBloomMips> bloomWidths_{};
  std::array<int, kBloomMips> bloomHeights_{};

  ParticleRing current_;
  ParticleRing previous_;
  std::vector<OutputPlane> outputs_;
  std::array<uint32_t, 2> timerQueries_{};
  // Which query the current frame writes to. The other one is read, giving the
  // GPU a full frame to retire it.
  size_t timerSlot_ = 0;
  bool timerPrimed_ = false;
  bool timerActive_ = false;
  RenderStats stats_;
};

}  // namespace nova::gfx
