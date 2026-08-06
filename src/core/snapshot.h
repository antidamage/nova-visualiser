// Scene snapshot handed from the simulation thread to the render thread.
//
// The layout of `RenderParticle` is the GPU vertex-instance layout: it is
// uploaded verbatim, so it must stay 16-byte aligned and match
// `src/shaders/particle.vert`. It also matches tvOS `PhonoscopeGPUParticle`
// field-for-field, which keeps the two shaders comparable.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/effect_scale.h"
#include "core/image.h"
#include "core/signal.h"
#include "core/vec.h"

namespace nova {

struct RenderParticle {
  Vec4 positionSize{0, 0, 0, 0};  // xyz world position, w radius
  Vec4 color{0, 0, 0, 0};         // gradient start
  Vec4 colorEnd{0, 0, 0, 0};      // gradient end
  Vec4 glowColor{0, 0, 0, 0};
  Vec4 glowColorEnd{0, 0, 0, 0};
  Vec4 meta{0, 0, 0, 0};   // x glow, y primitive, z material, w unused
  Vec4 trail{0, 0, 0, 0};  // xyz direction, w length
};
static_assert(sizeof(RenderParticle) == 112, "RenderParticle must match the GPU layout");

struct Diagnostics {
  double simulationMilliseconds = 0;
  int propagationDeliveries = 0;
  int droppedEffects = 0;
  int roundTrips = 0;
  int entityCount = 0;
  int particleCount = 0;
};

struct SceneSnapshot {
  uint64_t serial = 0;
  // Bumped on every structural rebuild. The renderer only interpolates between
  // two snapshots that share this value, so a rebuild snaps instead of
  // smearing particles between unrelated scenes.
  uint64_t structureSerial = 0;
  double simulationTime = 0;
  std::vector<RenderParticle> particles;
  Vec4 background{0, 0, 0, 1};
  Vec4 fluidBackground{0, 0, 0, 1};
  Vec4 fluidAccent{0.45f, 0.45f, 0.45f, 1};
  Vec4 fluidHighlight{0.85f, 0.85f, 0.85f, 1};
  float fluidSpeed = 1.0f;
  // Frame geometry and vignette. All driven except the colour, which is the
  // theme's `vignette` palette slot. The defaults are the original fixed
  // letterbox: a centred band one third high, full width, framed by the
  // authored black edge gradients.
  float backgroundHeight = 1.0f / 3.0f;
  float backgroundWidth = 1.0f;
  Vec4 vignetteColor{0, 0, 0, 1};
  float vignetteOpacity = 0.96f;
  float vignetteSize = 1.0f;
  // How the scene layer meets the backdrop, already snapped off the driven
  // `__sceneBlend` axis by `sceneBlendModeFor`. Linear is the original term.
  SceneBlendMode sceneBlendMode = SceneBlendMode::Linear;
  // The centre slot. Exactly one of `message` and `centreImage` is ever
  // non-empty in a given frame -- which of them is `Simulation::submit`'s
  // decision, not the renderer's.
  std::string message;
  // The incoming centre image, and the one still fading out behind it during a
  // colour-theme change. Shared pointers rather than pixels: the snapshot is
  // copied every tick, and the renderer's "is this the same image?" check is a
  // pointer comparison rather than a hash of several megabytes.
  std::shared_ptr<const DecodedImage> centreImage;
  std::shared_ptr<const DecodedImage> centreImageFrom;
  // The incoming image's weight, 0 to 1. 1 whenever nothing is cross-fading.
  float centreImageFade = 1.0f;
  // How tall the centre image is drawn, as a fraction of the frame. Width
  // follows from the source's proportions, so the picture is never distorted.
  float centreImageHeight = 0.33f;
  // Shared by both: the slot is scaled, not whatever happens to be in it.
  float messageScale = 1.0f;
  Vec4 messageColor{1, 1, 1, 1};
  // Final glow-overlay pass, applied over the whole picture including the
  // message. Authored 0-20 and 0-100 in the dashboard; both are fully driven
  // parameters, so they arrive already resolved for this frame.
  float glowBlurAmount = 0.0f;
  float glowOpacity = 0.0f;
  // 1-10, multiplied into the blurred copy.
  float glowOverdrive = 1.0f;
  // Whether the overdriven glow is brought back into 0-1 before the blend.
  bool glowClamped = true;
  // Photoshop's "screen", "multiply" or "overlay", already snapped off the
  // driven `__glowBlend` axis by `glowBlendModeFor`.
  GlowBlendMode glowBlendMode = GlowBlendMode::Screen;
  Vec3 boundsMinimum{-1.7778f, -1, 0};
  Vec3 boundsMaximum{1.7778f, 1, 0};
  bool is3D = false;
  SignalFrame signal;
  Diagnostics diagnostics;
};

using SnapshotPtr = std::shared_ptr<const SceneSnapshot>;

// Lock-free-enough handoff: the producer swaps a shared_ptr, consumers take a
// copy. The render thread never blocks on the simulation thread, which is the
// property `PHONOSCOPE_MODULE_SPEC.md` section 11 requires.
class SnapshotChannel {
 public:
  void publish(SnapshotPtr snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    previous_ = std::move(latest_);
    latest_ = std::move(snapshot);
  }

  // Returns the newest snapshot and the one before it, so the renderer can
  // interpolate rather than showing whichever tick happened to land last.
  void latestPair(SnapshotPtr& latest, SnapshotPtr& previous) const {
    std::lock_guard<std::mutex> lock(mutex_);
    latest = latest_;
    previous = previous_;
  }

  SnapshotPtr latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
  }

 private:
  mutable std::mutex mutex_;
  SnapshotPtr latest_;
  SnapshotPtr previous_;
};

}  // namespace nova
