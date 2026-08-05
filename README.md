# nova-visualiser

Headless GPU renderer for the Nova Phonoscope, running on **iridium** and
streamed to the Apple TV.

The Apple TV used to run the whole visualiser itself — expression VM, scene
graph, particle simulation and Metal renderer, all on an A10X. It cannot reach
4K60 at the module spec's ceilings, and the tvOS renderer rebuilt and re-uploaded
every particle on every draw. This service moves the engine onto iridium's
RTX 2080 Ti, hardware-encodes the result, and turns the Apple TV into a thin
client that shows video and sends commands back.

**The Apple TV keeps its Metal engine permanently as a fallback.** Two
independent implementations of `PHONOSCOPE_MODULE_SPEC.md` now exist, so
`tests/conformance` exists to stop them drifting.

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

## Why these choices

| Decision | Reason |
|---|---|
| **GLSL, not HLSL** | Linux + NVIDIA: GLSL is first-class and `glslang` is packaged. HLSL would mean adding DXC for no gain — there is no D3D target. Modules never carry shader source, so this is engine-internal either way. |
| **OpenGL 4.6 via headless EGL, not Vulkan** | `EGL_EXT_platform_device` gives a GPU context with no X server. The workload is one instanced draw plus a few fullscreen passes; Vulkan's submission parallelism would buy nothing for several thousand lines of boilerplate. The NVIDIA Vulkan ICD is installed anyway if that ever changes. |
| **HEVC Main10 for the Apple TV** | AppleTV6,2 (Apple TV 4K, A10X) decodes HEVC Main10 4K60 in hardware and has no AV1 decoder; Turing NVENC has no AV1 encoder. 10-bit is deliberate: this visualiser is almost entirely smooth gradients and bloom, which band badly at 8-bit. |
| **Bespoke TCP, not HLS** | Even low-latency HLS adds segmenting, playlists and AVPlayer buffering. Feeding `AVSampleBufferDisplayLayer` directly is hardware decode with about one frame of pipeline. |
| **H.264 + MediaMTX for browsers** | Browsers cannot decode HEVC over WebRTC. The sidecar supplies WHEP and LL-HLS so this service never has to implement SDP/ICE/DTLS/SRTP. |
| **Fixed 1/120 s simulation step** | Makes simulation outcome independent of render rate, lets the renderer interpolate, and is what makes the conformance corpus reproducible at all. |

## Threading

| Thread | Owns |
|---|---|
| config | HTTP + SSE against the dashboard; publishes immutable config snapshots |
| simulation | fixed 1/120 s accumulator; publishes scene snapshots; never blocks on the renderer |
| render | the sole GL context, plus CUDA interop and NVENC (`cuGraphicsMapResources` needs the GL context current on the calling thread). Paced by a monotonic deadline — there is no display and therefore no vsync |
| writers | one per stream client; a slow client is dropped to the next IDR and never applies back-pressure to the GPU |
| lighting | house-party frames to the dashboard |
| control | status and commands |

## Stability

The GPU is shared with the voice stack, which holds ~6.5 GB of the card's 11 GB.
So:

- this is its own systemd unit; **nothing depends on it and it depends on
  nothing**. Stopping, crashing or upgrading it cannot affect home control or
  the voice assistant;
- GL and encoder resources are **released when no client is connected**, giving
  the VRAM back between parties;
- dynamic resolution scales the *scene* only. The encode surface stays 4K, so
  the stream resolution never changes under a client. It reacts to render cost
  alone — shrinking the scene does nothing for an encoder bottleneck;
- shutdown has a watchdog, because CUDA teardown can deadlock inside
  `libnvcuvid` and a unit stuck in `deactivating` on this box is unacceptable.

## Building and deploying

```powershell
# From the repo root, in PowerShell (not Git Bash).
.\deploy-nova-visualiser.ps1              # build, conformance, probe, install, start
.\deploy-nova-visualiser.ps1 -SelfTestOnly # build and measure, install nothing
```

The build happens natively on iridium — it is the only Linux box with the GPU —
matching the dashboard's build-on-host pattern. Iridium's login shell is fish,
so remote scripts are base64-encoded and piped into `bash -s`.

### Prerequisites on iridium

The box ships the **compute-only** NVIDIA driver, which has no NVENC and no
EGL/GL. Add the matching userspace at the **exact same version** as the running
kernel module:

```sh
sudo apt-get install libnvidia-gl-580-server libnvidia-encode-580-server
```

Matching the version matters: a mismatch breaks CUDA for the voice stack. This
install is purely additive — it upgrades nothing and reloads no kernel module.

Build tooling: `cmake ninja-build g++ glslang-tools libavcodec-dev
libavformat-dev libavutil-dev libepoxy-dev libegl-dev pkg-config`.

## Checking it

```sh
curl -s http://iridium.local:8771/status | python3 -m json.tool
/opt/nova-visualiser/bin/nova-visualiser-probe          # GPU capability
/opt/nova-visualiser/bin/nova-visualiser-conformance \
    --corpus /opt/nova-visualiser/tests/conformance     # cross-engine parity
```

Offline modes, useful when something looks wrong:

```sh
nova-visualiser --self-test 300 --publish ""     # render + encode timing
nova-visualiser --dump-frame /tmp/frame.ppm      # one composited frame + stage report
```

`--dump-frame` reports the brightness of each pass separately in colour and
alpha, which is how a dark frame gets diagnosed rather than guessed at.

## Conformance

The Apple TV runs its own Metal implementation of the same module spec, so the
two will drift unless drift is detectable.

```sh
nova-visualiser-conformance --corpus tests/conformance
nova-visualiser-conformance --corpus tests/conformance --update   # rebaseline
```

Each case is a compiled module plus a fixed input trace; the runner emits
per-tick digests of the particle state. **Only `--update` when a spec change is
intended, and update the tvOS side in the same commit.**

Determinism required one behavioural change on both engines: the per-module
random seed is now an explicit FNV-1a over the module id, not Swift's
`String.hashValue`, which is salted per process and so differed between launches.
