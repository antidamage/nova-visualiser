#include "encode/cuda_interop.h"

#include <cuda.h>
#include <cudaGL.h>

namespace nova::encode {
namespace {

std::string describe(CUresult result, const char* what) {
  const char* name = nullptr;
  cuGetErrorName(result, &name);
  return std::string(what) + ": " + (name != nullptr ? name : "unknown CUDA error");
}

struct ContextScope {
  explicit ContextScope(CUcontext context) { cuCtxPushCurrent(context); }
  ~ContextScope() { cuCtxPopCurrent(nullptr); }
};

}  // namespace

CudaInterop::~CudaInterop() { shutdown(); }

bool CudaInterop::initialise(std::string& error) {
  CUresult result = cuInit(0);
  if (result != CUDA_SUCCESS) {
    error = describe(result, "cuInit");
    return false;
  }

  unsigned int deviceCount = 0;
  CUdevice glDevice = 0;
  result = cuGLGetDevices(&deviceCount, &glDevice, 1, CU_GL_DEVICE_LIST_ALL);
  if (result != CUDA_SUCCESS || deviceCount == 0) {
    error = describe(result, "cuGLGetDevices (is a GL context current?)");
    return false;
  }
  device_ = glDevice;

  result = cuDevicePrimaryCtxRetain(&context_, glDevice);
  if (result != CUDA_SUCCESS) {
    error = describe(result, "cuDevicePrimaryCtxRetain");
    return false;
  }
  ownsContext_ = true;
  return true;
}

bool CudaInterop::registerBuffer(uint32_t glBuffer, std::string& error) {
  if (resources_.count(glBuffer) > 0) return true;
  ContextScope scope(context_);
  CUgraphicsResource resource = nullptr;
  const CUresult result =
      cuGraphicsGLRegisterBuffer(&resource, glBuffer, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
  if (result != CUDA_SUCCESS) {
    error = describe(result, "cuGraphicsGLRegisterBuffer");
    return false;
  }
  resources_[glBuffer] = resource;
  return true;
}

void CudaInterop::unregisterBuffer(uint32_t glBuffer) {
  auto it = resources_.find(glBuffer);
  if (it == resources_.end()) return;
  ContextScope scope(context_);
  cuGraphicsUnregisterResource(it->second);
  resources_.erase(it);
}

bool CudaInterop::copyToDevice(uint32_t glBuffer, void* destination, size_t destinationPitch,
                               size_t sourcePitch, size_t rowBytes, size_t rows,
                               std::string& error) {
  auto it = resources_.find(glBuffer);
  if (it == resources_.end()) {
    error = "GL buffer is not registered with CUDA";
    return false;
  }

  ContextScope scope(context_);
  CUgraphicsResource resource = it->second;
  CUresult result = cuGraphicsMapResources(1, &resource, nullptr);
  if (result != CUDA_SUCCESS) {
    error = describe(result, "cuGraphicsMapResources");
    return false;
  }

  CUdeviceptr source = 0;
  size_t size = 0;
  result = cuGraphicsResourceGetMappedPointer(&source, &size, resource);
  if (result != CUDA_SUCCESS) {
    cuGraphicsUnmapResources(1, &resource, nullptr);
    error = describe(result, "cuGraphicsResourceGetMappedPointer");
    return false;
  }

  CUDA_MEMCPY2D copy{};
  copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  copy.srcDevice = source;
  copy.srcPitch = sourcePitch;
  copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  copy.dstDevice = reinterpret_cast<CUdeviceptr>(destination);
  copy.dstPitch = destinationPitch;
  copy.WidthInBytes = rowBytes;
  copy.Height = rows;
  result = cuMemcpy2D(&copy);

  cuGraphicsUnmapResources(1, &resource, nullptr);
  if (result != CUDA_SUCCESS) {
    error = describe(result, "cuMemcpy2D");
    return false;
  }
  return true;
}

bool CudaInterop::synchronise(std::string& error) {
  ContextScope scope(context_);
  const CUresult result = cuCtxSynchronize();
  if (result != CUDA_SUCCESS) {
    error = describe(result, "cuCtxSynchronize");
    return false;
  }
  return true;
}

void CudaInterop::shutdown() {
  if (context_ == nullptr) return;
  {
    ContextScope scope(context_);
    for (auto& [buffer, resource] : resources_) cuGraphicsUnregisterResource(resource);
  }
  resources_.clear();
  if (ownsContext_) cuDevicePrimaryCtxRelease(device_);
  context_ = nullptr;
  ownsContext_ = false;
}

}  // namespace nova::encode
