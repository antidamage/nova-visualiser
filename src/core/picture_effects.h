// The picture-level effects: household configuration declared by no module
// manifest, resolved through exactly the same driver lanes as every module
// setting.
//
// One table, because there are two places on this side that need the same
// answer and they used to hold their own copies:
//
//   - `net/config_client.cpp` seeds a value per effect. That seed is also the
//     parse-time test for "is this an effect this build knows about" -- a
//     binding naming anything else is dropped rather than carried as a binding
//     that can never resolve.
//   - `service/engine.cpp` declares each one's range to the lane evaluator.
//
// A private effect present in the second list but missing from the first is
// silently inert: the engine happily declares a range for it and no binding
// ever arrives, because the parse dropped every one. That is exactly what
// happened to `__glowOverdrive` and `__glowClamp`, which is why this table
// exists rather than two hand-maintained lists.
//
// Mirrors PHONOSCOPE_PICTURE_EFFECTS in the dashboard (the reference) and the
// declarations in `refreshResolvedSettings` in PhonoscopeStore.swift. All three
// must agree on every range.
//
// `__hueOffset` is here because this engine resolves it -- only this side holds
// the spectrum a bass or energy driver reads, and the value rides out on the
// House Party lighting frame. `__themeChange` and `__altTheme` are NOT: the
// dashboard owns the rotation and resolves them itself.
#pragma once

#include <array>

#include "core/centre_image_reference.h"
#include "core/centre_image_transition.h"
#include "core/effect_scale.h"
#include "core/image_fit_reference.h"

namespace nova {

struct PictureEffect {
  const char* id;
  double min;
  double max;
  double step;
  double defaultValue;
};

inline constexpr std::array<PictureEffect, 27> kPictureEffects = {{
    {"__messageScale", kImageScaleMinimum, kImageScaleMaximum, 0.1, 1.0},
    // The centre image's base size, as percentages of the frame. A separate axis
    // from the scale above: this is how big the image is, that is a multiplier a
    // driver lane can sweep on top of it.
    //
    // Width is the AUTHORED axis and height follows it while `__centreProportional`
    // is on, which is the default and is what the slot did before it had a width.
    {"__centreWidth", 0.0, 100.0, 1.0, kCentreImageDefaultHeightPercent},
    {"__centreHeight", 0.0, 100.0, 1.0, kCentreImageDefaultHeightPercent},
    // 0 manual, 1 fit to screen, 2 fill screen, snapped by `imageFitFor`.
    {"__centreFit", 0.0, static_cast<double>(kImageFitModeCount - 1), 1.0, 0.0},
    {"__centreProportional", 0.0, 1.0, 1.0, 1.0},
    {"__glowBlur", 0.0, 20.0, 0.1, 0.0},
    // Opacity 0 is the identity, and it is what both engines check to skip the
    // glow pass entirely.
    {"__glowOpacity", 0.0, 100.0, 1.0, 0.0},
    // 1 is the identity: the glow is used exactly as blurred.
    {"__glowOverdrive", 1.0, 10.0, 0.1, 1.0},
    // 0/1: clamped by default, which is the display-referred behaviour.
    {"__glowClamp", 0.0, 1.0, 1.0, 1.0},
    // 0 screen, 1 multiply, 2 overlay, snapped by `glowBlendModeFor`. A step of
    // 1 keeps every authored endpoint on a real mode.
    {"__glowBlend", 0.0, static_cast<double>(kGlowBlendModeCount - 1), 1.0, 0.0},
    // Degrees of random hue jitter per House Party light.
    {"__hueOffset", 0.0, 180.0, 1.0, 5.0},
    // Frame geometry, as a PERCENTAGE of the render view. The defaults are the
    // fixed letterbox these replaced: a centred band one third high and full
    // width. Authored 0-100 because "33%" is what the control means; the divide
    // by 100 happens once, where the value is clamped in Simulation::submit, so
    // the snapshot, the shaders and the recorded band digests stay in unit
    // space.
    {"__bgHeight", 0.0, 100.0, 1.0, 33.0},
    {"__bgWidth", 0.0, 100.0, 1.0, 100.0},
    // A multiplier on top of the two above, in every fit mode. 1 is the
    // identity, so an existing band is exactly the band it was. Shares the
    // centre scale's bounds because it is the same kind of axis.
    {"__bgScale", kImageScaleMinimum, kImageScaleMaximum, 0.1, 1.0},
    // 0 manual, 1 fit to screen, 2 fill screen. Only meaningful when the colour
    // theme names a background image -- a procedural field has no proportions to
    // fit -- and the dashboard hides the control until one does.
    {"__bgFit", 0.0, static_cast<double>(kImageFitModeCount - 1), 1.0, 0.0},
    {"__bgProportional", 0.0, 1.0, 1.0, 1.0},
    // Vignette. 96% and 1.0 are the authored `PhonoscopeEdgeVignette` exactly,
    // so an undriven frame is the one that was always drawn. Size can go past
    // 1 -- that is how the vignette closes the band down to a slit -- and stays
    // a multiplier rather than a percentage for exactly that reason.
    {"__vignetteOpacity", 0.0, 100.0, 1.0, 96.0},
    {"__vignetteSize", 0.0, 3.0, 0.05, 1.0},
    // 0 linear, 1 screen, 2 overlay, 3 multiply, snapped by `sceneBlendModeFor`.
    // Linear is the original composite term and so the default.
    {"__sceneBlend", 0.0, static_cast<double>(kSceneBlendModeCount - 1), 1.0, 0.0},
    // The centre-image transition. Resolved like any other axis so a binding on
    // one of them is not dropped at parse, but deliberately NOT read back out
    // of the resolved settings: the dashboard latches these when the change
    // fires and publishes the answer on the theme state, because the initiator
    // owns the transition and this side has no way to know which entry a change
    // started from.
    {"__centreTransition", 0.0, 2.0, 1.0, 0.0},
    {"__centreTransitionAxis", 0.0, 360.0, 1.0, 0.0},
    {"__centreTransitionDivisions", 0.0, static_cast<double>(kCentreTransitionMaxDivisions), 1.0,
     0.0},
    {"__centreTransitionReturn", 0.0, 1.0, 1.0, 0.0},
    // The same four for the background image, with the same ranges, defaults
    // and latching rule. A backdrop and a centrepiece change at the same moment
    // but are not the same picture: dissolving one while the other slides is a
    // combination worth being able to author, and that needs its own axes.
    {"__bgTransition", 0.0, 2.0, 1.0, 0.0},
    {"__bgTransitionAxis", 0.0, 360.0, 1.0, 0.0},
    {"__bgTransitionDivisions", 0.0, static_cast<double>(kCentreTransitionMaxDivisions), 1.0, 0.0},
    {"__bgTransitionReturn", 0.0, 1.0, 1.0, 0.0},
}};

}  // namespace nova
