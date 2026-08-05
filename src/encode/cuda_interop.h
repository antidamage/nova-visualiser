// CUDA <-> OpenGL bridge.
//
// The renderer writes finished YUV planes into GL buffer objects; NVENC reads
// CUDA device memory. Registering the GL buffers with CUDA lets the frame reach
// the encoder without ever touching host memory: the only movement is a
// device-to-device pitch copy into the surface libavcodec owns, which at 4K
// P010 is ~25 MB/frame -- around 0.25% of this GPU's bandwidth.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

typedef struct CUctx_st* CUcontext;
typedef struct CUgraphicsResource_st* CUgraphicsResource;

namespace nova::encode {

class CudaInterop {
 public:
  CudaInterop() = default;
  ~CudaInterop();

  CudaInterop(const CudaInterop&) = delete;
  CudaInterop& operator=(const CudaInterop&) = delete;

  // Binds to the CUDA device that owns the current GL context, so interop can
  // never silently land on a different GPU.
  bool initialise(std::string& error);
  void shutdown();

  CUcontext context() const { return context_; }

  bool registerBuffer(uint32_t glBuffer, std::string& error);
  void unregisterBuffer(uint32_t glBuffer);

  // Copies a registered GL buffer into device memory owned by someone else
  // (libavcodec's CUDA frame). `sourcePitch` and `destinationPitch` are in
  // bytes; `rowBytes` is the meaningful width.
  bool copyToDevice(uint32_t glBuffer, void* destination, size_t destinationPitch,
                    size_t sourcePitch, size_t rowBytes, size_t rows, std::string& error);

  bool synchronise(std::string& error);

 private:
  CUcontext context_ = nullptr;
  bool ownsContext_ = false;
  int device_ = 0;
  std::unordered_map<uint32_t, CUgraphicsResource> resources_;
};

}  // namespace nova::encode
