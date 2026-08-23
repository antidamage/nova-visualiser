# Backdrop transitions

How the picture's backdrop changes when the colour theme changes.

Written from plan `cuddly-spinning-waffle` (2026-08-23), after the report that
the visualiser cut instantly between "no background" and a background image
instead of respecting the change-theme effect's ramp.

Both rendering engines implement this: `src/shaders/fluid_background.frag`
here, and `phonoscope_fluid_background` in
`nova-appletv-dashboard/NovaAppleTVDashboard/FluidBackgroundShader.metal`.
Changing the rules below means changing both, plus the reference header,
the conformance expectation, and the tvOS parity test — see "Locked by".

## 1. The backdrop slot has two occupants

The backdrop is ONE slot, and exactly one of two things occupies it:

| occupant | when | drawn as |
|---|---|---|
| a background image | the live colour theme names one | the fitted image over the theme's backdrop colour, framed whole-frame |
| the procedural field | the theme names no image | the blob band at its authored height and width, framed band-locally |

A null background image is **the field**, not "nothing". This is the whole
basis of the rest of this document: a change from an image to null is a change
of occupant exactly like a change from one image to another, and gets the same
treatment.

The two occupants are framed differently and that is intended: an image has no
band, so the vignette closes over the whole frame around it, while the field
is clipped to the band with the bars outside it solid vignette colour.

## 2. When a transition runs

A transition starts when **the occupant changes** — image → image, image →
field, field → image — and the authored ramp has non-zero length.

- Both ends may be null. Field → field is not a change and starts nothing.
- Image identity is by pointer, not by contents: two entries naming the same
  library image are the same decoded buffer and moving between them is not a
  transition.
- A ramp of zero total length is a **cut**: the change lands on the next frame.
- **First paint dissolves.** Unlike the centre slot, whose rule is "a first
  paint should not fly on from off screen", the backdrop's first image
  dissolves in from the field — the field is genuinely on screen before it
  arrives, so fading it out is honest rather than invented.
- A transition that has begun always finishes, including while the rotation
  is paused. Pausing means "stop advancing the playlist", not "freeze a change
  already in flight".

The ramp itself is `transitionRamp` in `src/core/centre_image_transition.h`
read as a motion profile — attack eases in, hold is the flat middle, release
eases out — and the transition lasts exactly attack + hold + release.

The transition's parameters are **latched at the instant the occupant
changes** and held for the run. The entry being LEFT owns the change; a
parameter edited mid-flight lands on the next change rather than tearing the
one already running.

## 3. What is composited

Name the two sides `from` (the occupant leaving) and `to` (the occupant
arriving), and `w` the ramp's progress, 0 to 1.

Each side resolves to a **finished, framed picture**:

```
side(occupant) = occupant is an image
    ? framedBackdrop(imagePlane(occupant) over imageBackdrop, frameUv, 1.0)
    : fieldBackdrop(frameUv)
```

and the result is a straight cross-dissolve of the two:

```
result = mix(side(from), side(to), w)
```

Framings are cross-dissolved, **not morphed**. The band does not widen into
the frame as an image arrives; the letterboxed picture dissolves into the
full-frame picture. Both endpoints are therefore pixel-identical to what each
occupant looks like on its own, which is the property the conformance
expectation relies on.

### The image ↔ image exception

When BOTH sides are images and the authored transition mode is Flip or Slide,
the existing single-plane rule stands unchanged: exactly one plane is drawn,
and the swap is the exact midpoint. That instant is what makes a flip read as
one object turning over rather than two images blending through each other.

### Field ↔ image is always a cross-dissolve

When either side is the field, the change is a cross-dissolve whatever the
mode axis names. The field has no rectangle to flip or slide — there is
nothing for the geometry to act on — so the mode is ignored rather than
half-applied.

Concretely this means the image plane is sampled with cross-fade geometry (no
collapse, no displacement) on a field ↔ image change, even when the axis says
Flip or Slide.

### Degenerate cases

- `w` at 0 or 1 must short-circuit to a single side, so the steady state costs
  exactly what it cost before this existed.
- Both sides null cannot happen (§2), but if it did it is the field.

## 4. What "done" means

All four cases behave as the table says, verified live on the visualiser
stream AND on the Apple TV, with a slow, obviously-visible ramp:

| case | expected |
|---|---|
| field → image | the band dissolves out as the image dissolves in, over the ramp |
| image → field | the image dissolves out as the band dissolves in, over the ramp |
| image → image | unchanged: cross-fade, flip or slide per the authored mode |
| field → field | nothing happens |

Plus:

- the ramp's SHAPE is honoured, not merely "it fades": an attack-heavy ramp
  reads as the change accelerating in, a release-heavy one as it settling out;
- the steady state is unchanged — an image theme still fills the frame with
  the vignette over the whole picture, a no-image theme still shows the
  letterboxed band;
- every existing conformance case still passes, because no endpoint moved.

## Locked by

- `src/core/background_band_reference.h` — the presence/mix arithmetic, stated
  once for both engines.
- `src/tools/conformance.cpp`, `runBackgroundBandCase()`, recorded in
  `tests/conformance/background-band/expected.json`.
- `ParitySelfTests.testBackgroundBandParity()` on tvOS.

Changing any one of those means changing all of them.
