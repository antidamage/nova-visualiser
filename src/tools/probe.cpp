// Capability probe for the iridium GPU path.
//
// Phase 0 of the rebuild: prove headless GL, NVENC and CUDA-GL interop work on
// this box *before* any engine code depends on them, and prove it without
// disturbing the voice stack that shares the GPU. Run it again after any driver
// change.
//
//   nova-visualiser-probe [--width 3840] [--height 2160] [--frames 180]
// epoxy/gl.h must precede cudaGL.h: cudaGL.h pulls in GL/gl.h, and epoxy
// refuses to load after the system header has already defined the API.
#include <epoxy/gl.h>

#include <cuda.h>
#include <cudaGL.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
}

#include "gfx/egl_device.h"

namespace {

int failures = 0;

void pass(const std::string& what) { std::printf("  PASS  %s\n", what.c_str()); }

void fail(const std::string& what, const std::string& why) {
  std::printf("  FAIL  %s -- %s\n", what.c_str(), why.c_str());
  ++failures;
}

double seconds() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string cudaError(CUresult result) {
  const char* name = nullptr;
  cuGetErrorName(result, &name);
  return name != nullptr ? name : "unknown CUDA error";
}

// -------------------------------------------------------------------------
// Probe 1 -- headless OpenGL on the NVIDIA device.
// -------------------------------------------------------------------------
bool probeHeadlessGl(nova::gfx::EglDevice& device) {
  std::printf("\n[1] Headless EGL / OpenGL\n");
  std::string error;
  if (!device.initialise(-1, error)) {
    fail("EGL surfaceless context", error);
    return false;
  }
  std::printf("        vendor   : %s\n", device.vendor().c_str());
  std::printf("        renderer : %s\n", device.renderer().c_str());
  std::printf("        version  : %s\n", device.glVersion().c_str());
  if (device.renderer().find("NVIDIA") == std::string::npos) {
    fail("context is on the NVIDIA GPU", "renderer is " + device.renderer());
    return false;
  }
  pass("EGL surfaceless OpenGL 4.6 core context on the NVIDIA GPU");

  // A real render target at the target resolution, to prove the memory is
  // actually available alongside the voice stack.
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 3840, 2160);
  const GLenum status = glGetError();
  if (status != GL_NO_ERROR) {
    fail("allocate 3840x2160 RGBA16F target", "glGetError 0x" + std::to_string(status));
  } else {
    pass("allocate 3840x2160 RGBA16F render target (66 MB)");
  }
  glDeleteTextures(1, &texture);
  return true;
}

// -------------------------------------------------------------------------
// Probe 2 -- CUDA-GL interop with no host round trip.
// -------------------------------------------------------------------------
bool probeCudaGlInterop(CUcontext& cudaContext) {
  std::printf("\n[2] CUDA-GL interop\n");
  CUresult result = cuInit(0);
  if (result != CUDA_SUCCESS) {
    fail("cuInit", cudaError(result));
    return false;
  }

  // Bind the CUDA context to whichever device owns the current GL context, so
  // interop cannot silently land on a different GPU.
  unsigned int deviceCount = 0;
  CUdevice glDevice = 0;
  result = cuGLGetDevices(&deviceCount, &glDevice, 1, CU_GL_DEVICE_LIST_ALL);
  if (result != CUDA_SUCCESS || deviceCount == 0) {
    fail("cuGLGetDevices", cudaError(result));
    return false;
  }
  result = cuDevicePrimaryCtxRetain(&cudaContext, glDevice);
  if (result != CUDA_SUCCESS) {
    fail("cuDevicePrimaryCtxRetain", cudaError(result));
    return false;
  }
  cuCtxPushCurrent(cudaContext);
  pass("CUDA context bound to the GL device");

  constexpr size_t kBytes = 1024 * 1024;
  GLuint buffer = 0;
  glGenBuffers(1, &buffer);
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  glBufferData(GL_ARRAY_BUFFER, kBytes, nullptr, GL_DYNAMIC_COPY);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  CUgraphicsResource resource = nullptr;
  result = cuGraphicsGLRegisterBuffer(&resource, buffer, CU_GRAPHICS_REGISTER_FLAGS_NONE);
  if (result != CUDA_SUCCESS) {
    fail("cuGraphicsGLRegisterBuffer", cudaError(result));
    glDeleteBuffers(1, &buffer);
    return false;
  }

  bool ok = true;
  result = cuGraphicsMapResources(1, &resource, nullptr);
  if (result != CUDA_SUCCESS) {
    fail("cuGraphicsMapResources", cudaError(result));
    ok = false;
  } else {
    CUdeviceptr pointer = 0;
    size_t size = 0;
    result = cuGraphicsResourceGetMappedPointer(&pointer, &size, resource);
    if (result != CUDA_SUCCESS || size < kBytes) {
      fail("cuGraphicsResourceGetMappedPointer", cudaError(result));
      ok = false;
    } else {
      cuMemsetD8(pointer, 0xA5, kBytes);
      cuCtxSynchronize();
    }
    cuGraphicsUnmapResources(1, &resource, nullptr);
  }

  if (ok) {
    // Read back through GL: if CUDA wrote into the GL allocation the pattern is
    // visible without any explicit copy between the two APIs.
    std::vector<unsigned char> readback(256);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(readback.size()),
                       readback.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const bool matched = readback[0] == 0xA5 && readback[255] == 0xA5;
    if (matched) {
      pass("CUDA wrote directly into GL-owned memory (zero host copies)");
    } else {
      fail("CUDA-GL shared memory round trip", "readback pattern did not match");
      ok = false;
    }
  }

  cuGraphicsUnregisterResource(resource);
  glDeleteBuffers(1, &buffer);
  return ok;
}

// -------------------------------------------------------------------------
// Probe 3 -- NVENC throughput at the Apple TV rung.
// -------------------------------------------------------------------------
bool probeNvenc(CUcontext cudaContext, int width, int height, int frames) {
  std::printf("\n[3] NVENC HEVC %dx%d\n", width, height);

  const AVCodec* codec = avcodec_find_encoder_by_name("hevc_nvenc");
  if (codec == nullptr) {
    fail("find hevc_nvenc", "encoder not present in this libavcodec");
    return false;
  }

  AVBufferRef* deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
  if (deviceRef == nullptr) {
    fail("av_hwdevice_ctx_alloc", "out of memory");
    return false;
  }
  auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(deviceRef->data);
  auto* cudaDeviceContext = static_cast<AVCUDADeviceContext*>(deviceContext->hwctx);
  cudaDeviceContext->cuda_ctx = cudaContext;
  if (av_hwdevice_ctx_init(deviceRef) < 0) {
    fail("av_hwdevice_ctx_init", "could not adopt the existing CUDA context");
    av_buffer_unref(&deviceRef);
    return false;
  }

  AVBufferRef* framesRef = av_hwframe_ctx_alloc(deviceRef);
  auto* framesContext = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
  framesContext->format = AV_PIX_FMT_CUDA;
  framesContext->sw_format = AV_PIX_FMT_P010LE;  // Main10: the visualiser is all gradients
  framesContext->width = width;
  framesContext->height = height;
  framesContext->initial_pool_size = 8;
  if (av_hwframe_ctx_init(framesRef) < 0) {
    fail("av_hwframe_ctx_init", "could not allocate a CUDA frame pool");
    av_buffer_unref(&framesRef);
    av_buffer_unref(&deviceRef);
    return false;
  }
  pass("CUDA P010 frame pool allocated");

  AVCodecContext* encoder = avcodec_alloc_context3(codec);
  encoder->width = width;
  encoder->height = height;
  encoder->pix_fmt = AV_PIX_FMT_CUDA;
  encoder->sw_pix_fmt = AV_PIX_FMT_P010LE;
  encoder->time_base = AVRational{1, 60};
  encoder->framerate = AVRational{60, 1};
  encoder->bit_rate = 30'000'000;
  encoder->rc_max_rate = 30'000'000;
  encoder->gop_size = 120;
  encoder->max_b_frames = 0;
  encoder->hw_frames_ctx = av_buffer_ref(framesRef);
  av_opt_set(encoder->priv_data, "preset", "p4", 0);
  av_opt_set(encoder->priv_data, "tune", "ull", 0);
  av_opt_set(encoder->priv_data, "rc", "cbr", 0);
  av_opt_set(encoder->priv_data, "profile", "main10", 0);
  av_opt_set_int(encoder->priv_data, "delay", 0, 0);
  av_opt_set_int(encoder->priv_data, "zerolatency", 1, 0);

  if (avcodec_open2(encoder, codec, nullptr) < 0) {
    fail("avcodec_open2(hevc_nvenc)",
         "NVENC session could not be opened -- libnvidia-encode missing, or the "
         "GPU is out of encode sessions");
    avcodec_free_context(&encoder);
    av_buffer_unref(&framesRef);
    av_buffer_unref(&deviceRef);
    return false;
  }
  pass("NVENC HEVC Main10 session opened");

  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_CUDA;
  frame->width = width;
  frame->height = height;
  if (av_hwframe_get_buffer(framesRef, frame, 0) < 0) {
    fail("av_hwframe_get_buffer", "could not take a CUDA surface from the pool");
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
    av_buffer_unref(&framesRef);
    av_buffer_unref(&deviceRef);
    return false;
  }

  AVPacket* packet = av_packet_alloc();
  int64_t encodedBytes = 0;
  int encodedFrames = 0;
  const double started = seconds();

  cuCtxPushCurrent(cudaContext);
  for (int index = 0; index < frames; ++index) {
    // Vary the surface so the encoder is not handed an identical picture every
    // time, which would make the measurement meaninglessly optimistic.
    const unsigned char luma = static_cast<unsigned char>(16 + (index * 7) % 200);
    cuMemsetD2D8(reinterpret_cast<CUdeviceptr>(frame->data[0]), frame->linesize[0], luma,
                 static_cast<size_t>(width) * 2, static_cast<size_t>(height));
    frame->pts = index;
    if (avcodec_send_frame(encoder, frame) < 0) break;
    while (avcodec_receive_packet(encoder, packet) == 0) {
      encodedBytes += packet->size;
      ++encodedFrames;
      av_packet_unref(packet);
    }
  }
  avcodec_send_frame(encoder, nullptr);
  while (avcodec_receive_packet(encoder, packet) == 0) {
    encodedBytes += packet->size;
    ++encodedFrames;
    av_packet_unref(packet);
  }
  const double elapsed = seconds() - started;
  cuCtxPopCurrent(nullptr);

  const double fps = elapsed > 0 ? encodedFrames / elapsed : 0;
  std::printf("        %d frames in %.2f s = %.1f fps, %.1f Mbps\n", encodedFrames, elapsed, fps,
              elapsed > 0 ? (encodedBytes * 8.0 / elapsed) / 1e6 : 0.0);
  if (fps >= 60) {
    pass("sustains 60 fps at the Apple TV rung");
  } else {
    fail("sustains 60 fps", "measured " + std::to_string(static_cast<int>(fps)) + " fps");
  }

  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&encoder);
  av_buffer_unref(&framesRef);
  av_buffer_unref(&deviceRef);
  return fps >= 60;
}

}  // namespace

int main(int argc, char** argv) {
  int width = 3840;
  int height = 2160;
  int frames = 180;
  for (int index = 1; index + 1 < argc; index += 2) {
    const std::string flag = argv[index];
    const int value = std::atoi(argv[index + 1]);
    if (flag == "--width") width = value;
    if (flag == "--height") height = value;
    if (flag == "--frames") frames = value;
  }

  std::printf("nova-visualiser capability probe\n");
  std::printf("================================\n");

  nova::gfx::EglDevice device;
  if (!probeHeadlessGl(device)) {
    std::printf("\n%d check(s) failed.\n", failures);
    return 1;
  }

  CUcontext cudaContext = nullptr;
  if (probeCudaGlInterop(cudaContext)) {
    probeNvenc(cudaContext, width, height, frames);
  }

  std::printf("\n%s\n", failures == 0 ? "All checks passed." : "SOME CHECKS FAILED.");
  return failures == 0 ? 0 : 1;
}
