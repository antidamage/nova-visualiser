# nova-visualiser

The GPU renderer for the Nova Phonoscope. It runs the Phonoscope engine headless
on the GPU host, renders at 4K60, hardware-encodes with NVENC, and streams the
result to the Apple TV and to browsers. Playback devices decode video rather than
running the simulation themselves.

## Where it fits

| Component | Interface |
|---|---|
| Apple TV ([nova-appletv-dashboard](https://github.com/antidamage/nova-apple-tv)) | Bespoke TCP → `AVSampleBufferDisplayLayer`, HEVC Main10 |
| Browsers and web dashboards | H.264 via a MediaMTX sidecar (WHEP, LL-HLS) |
| [Nova HA Dashboard](https://github.com/antidamage/nova-ha-dashboard) | Serves config over SSE; receives house-party lighting frames |
| Module sources | **nova-visualiser-modules**, validated against `tests/conformance` |

The Apple TV keeps its own Metal implementation of the same module spec
permanently, as the fallback when this service is unavailable. Two independent
implementations therefore exist, and `tests/conformance` compares per-tick
particle digests across both to detect drift.

## What it does

**Rendering.** One render serves every viewer. The target Apple TV's A10X cannot
reach 4K60 at the module spec's ceilings, so the engine runs on the GPU host
instead and the output is distributed as video.

**Encoding.** HEVC Main10 for the Apple TV, which decodes it in hardware. The
output is almost entirely smooth gradients and bloom, which band at 8-bit, so
10-bit is required rather than preferred. H.264 via the MediaMTX sidecar for
browsers, which cannot decode HEVC over WebRTC.

**Latency.** Access units are fed to the decoder directly rather than through HLS
segmenting and player buffering, costing roughly one frame of pipeline.

**Isolation.** Its own systemd unit, which nothing depends on and which depends
on nothing. Stopping, crashing or upgrading it cannot affect home control or the
voice stack.

**Nothing to draw means nothing sent.** A renderer that has never managed to read
a configuration sends no frames at all rather than a flat one. A client that
takes a frame hides its own implementation in favour of it, so a blank picture
here is a black screen there; sending nothing keeps the Apple TV's own engine on
screen, which is what it is for. The reason is in `/status` as `configError`, and
in `/healthz` as `config: false` — that is the field which separates "nothing to
draw" from "nothing to watch". `ok` in the same payload follows GPU residency
alone, so an idle renderer that has released the card answers `ok: false` with a
503: measured on the host, 503 while idle and 200 with one client attached.

**Dashboard link.** Module selection, colours and settings come from the
dashboard's own listener — `NOVA_VISUALISER_DASHBOARD`, default
`http://127.0.0.1:3001`. Never through the household's browser ingress: Caddy owns
port 80 and routes by Host header, so a request to `127.0.0.1` matches no site
block and comes back `200` with an **empty body** — which reads here as a
configuration that did not parse, so no module is loaded and nothing is drawn.
Point the setting at the address Caddy itself proxies to: in the dashboard's own
repo, `nova-ha-dashboard/ops/iridium/nova-ha-dashboard.service` sets `PORT=3001`
and `NOVA_BIND_HOST=127.0.0.1`. That listener is loopback-only, so it needs no
session and is unaffected by the auth gate in front of the browser surface.

**VRAM management.** GL and encoder resources are released when no client is
connected, by this service's own idle policy (`--idle-release`, default 30 s) —
nothing else asks it to release, and nothing else can take them from it.
Measured with `nvidia-smi` on the host while a client was connected: a few MB
while released, about 0.8 GB of the card's 11 GB while rendering, beside the
voice stack's roughly 6.5 GB.

**Client handling.** Each stream client gets its own writer thread. A slow client
is dropped to the next IDR and never applies back-pressure to the GPU.

## Install

The build happens natively on the GPU host — it is the only Linux box with the
card — matching the dashboard's build-on-host pattern.

### Prerequisites

The host ships the **compute-only** NVIDIA driver, which has no NVENC and no
EGL/GL. Add the matching userspace at the **exact same version** as the running
kernel module:

```sh
sudo apt-get install libnvidia-gl-580-server libnvidia-encode-580-server
```

> Matching the version matters: a mismatch breaks CUDA for the voice stack. This
> install is purely additive — it upgrades nothing and reloads no kernel module.

Build tooling:

```sh
sudo apt-get install cmake ninja-build g++ glslang-tools libavcodec-dev \
  libavformat-dev libavutil-dev libepoxy-dev libegl-dev libfreetype-dev \
  libpng-dev pkg-config
```

FreeType and libpng draw the centre slot — the message and the image
respectively. Both are required rather than optional: the centre slot is part of
the streamed picture, so a build that silently could not draw one would be a
parity regression against the Apple TV rather than a degraded transport.

### Build and deploy

```powershell
# From the repo root, in PowerShell (not Git Bash).
.\deploy-nova-visualiser.ps1               # build, conformance, probe, install, start
.\deploy-nova-visualiser.ps1 -SelfTestOnly # build and measure, install nothing
```

The deploy script runs the conformance corpus and a GPU capability probe before
installing anything.

## Verifying a running instance

```sh
curl -s http://iridium.local:8771/status | python3 -m json.tool
/opt/nova-visualiser/bin/nova-visualiser-probe          # GPU capability
/opt/nova-visualiser/bin/nova-visualiser-conformance \
    --corpus /opt/nova-visualiser/tests/conformance     # cross-engine parity
```

Offline modes for when something looks wrong:

```sh
nova-visualiser --self-test 300 --publish ""     # render + encode timing
nova-visualiser --dump-frame /tmp/frame.ppm      # one composited frame + stage report
```

`--dump-frame` reports each pass's brightness separately in colour and alpha,
which is how a dark frame gets diagnosed rather than guessed at.

## Conformance

```sh
nova-visualiser-conformance --corpus tests/conformance
nova-visualiser-conformance --corpus tests/conformance --update   # rebaseline
```

Each case is a compiled module plus a fixed input trace; the runner emits
per-tick digests of particle state. **Only pass `--update` when a spec change is
intended, and update the tvOS side in the same commit.**

Determinism requires the per-module random seed to be an explicit FNV-1a over the
module id on both engines. Swift's `String.hashValue` is salted per process and
differs between launches, so it cannot be used here.

## Layout

```
src/core/     engine: expression VM, module decode, fixed-step simulation
              — no GL, no CUDA, no network. Builds anywhere.
src/gfx/      headless EGL device, render graph, GL resources
src/shaders/  GLSL 4.60, embedded into the binary at build time
src/encode/   CUDA-GL interop, NVENC via libavcodec, RTSP publisher
src/net/      config/SSE client, stream server, control API, house-party producer
src/service/  threading, frame pacing, orchestration
src/tools/    capability probe, conformance runner
ops/          systemd units, MediaMTX config, defaults
tests/        conformance corpus
```

### Threads

| Thread | Owns |
|---|---|
| config | HTTP + SSE against the dashboard; publishes immutable config snapshots |
| simulation | Fixed 1/120 s accumulator; publishes scene snapshots; never blocks on the renderer |
| render | The sole GL context, plus CUDA interop and NVENC (`cuGraphicsMapResources` needs the GL context current on the calling thread). Paced by a monotonic deadline — there is no display and therefore no vsync |
| writers | One per stream client; a slow client is dropped to the next IDR |
| lighting | House-party frames to the dashboard |
| control | Status and commands |

### Constraints worth knowing before changing things

| Choice | Constraint behind it |
|---|---|
| **GLSL, not HLSL** | Linux + NVIDIA: GLSL is first-class and `glslang` is packaged; there is no D3D target. Modules never carry shader source, so this stays engine-internal. |
| **OpenGL 4.6 via headless EGL, not Vulkan** | `EGL_EXT_platform_device` gives a GPU context with no X server. The workload is one instanced draw plus a few fullscreen passes; Vulkan's submission parallelism buys nothing for thousands of lines of boilerplate. |
| **HEVC Main10** | The target Apple TV decodes HEVC Main10 4K60 in hardware and has no AV1 decoder; Turing NVENC has no AV1 encoder. |
| **Bespoke TCP, not HLS** | Even low-latency HLS adds segmenting, playlists and player buffering. |
| **H.264 + MediaMTX for browsers** | Browsers cannot decode HEVC over WebRTC. The sidecar supplies WHEP and LL-HLS so this service never implements SDP/ICE/DTLS/SRTP. |
| **Fixed 1/120 s simulation step** | Makes simulation outcome independent of render rate, lets the renderer interpolate, and is what makes the conformance corpus reproducible at all. |
| **Dynamic resolution scales the scene only** | The encode surface stays 4K, so stream resolution never changes under a client. It reacts to render cost alone — shrinking the scene does nothing for an encoder bottleneck. |
| **Shutdown has a watchdog** | CUDA teardown can deadlock inside `libnvcuvid`, and a unit stuck in `deactivating` on this host is unacceptable. |
