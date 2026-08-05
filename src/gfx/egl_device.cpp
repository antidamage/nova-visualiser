#include "gfx/egl_device.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <vector>

namespace nova::gfx {
namespace {

std::string glString(GLenum name) {
  const GLubyte* value = glGetString(name);
  return value != nullptr ? reinterpret_cast<const char*>(value) : std::string();
}

}  // namespace

EglDevice::~EglDevice() { shutdown(); }

bool EglDevice::initialise(int preferredCudaDevice, std::string& error) {
  auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(
      eglGetProcAddress("eglQueryDevicesEXT"));
  auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  auto queryDeviceAttrib = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
      eglGetProcAddress("eglQueryDeviceAttribEXT"));
  if (queryDevices == nullptr || getPlatformDisplay == nullptr) {
    error = "EGL_EXT_platform_device is unavailable; the NVIDIA GL driver "
            "(libnvidia-gl-*) is probably not installed";
    return false;
  }

  EGLint deviceCount = 0;
  if (queryDevices(0, nullptr, &deviceCount) != EGL_TRUE || deviceCount <= 0) {
    error = "eglQueryDevicesEXT reported no EGL devices";
    return false;
  }
  std::vector<EGLDeviceEXT> devices(static_cast<size_t>(deviceCount));
  if (queryDevices(deviceCount, devices.data(), &deviceCount) != EGL_TRUE) {
    error = "eglQueryDevicesEXT failed";
    return false;
  }

  for (EGLint index = 0; index < deviceCount; ++index) {
    // Prefer the requested CUDA ordinal so the renderer lands on the same GPU
    // as the encoder when a box ever has more than one.
    if (preferredCudaDevice >= 0 && queryDeviceAttrib != nullptr) {
      EGLAttrib cudaOrdinal = -1;
      // EGL_CUDA_DEVICE_NV == 0x323A
      if (queryDeviceAttrib(devices[static_cast<size_t>(index)], 0x323A, &cudaOrdinal) == EGL_TRUE &&
          static_cast<int>(cudaOrdinal) != preferredCudaDevice) {
        continue;
      }
    }

    EGLDisplay display =
        getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[static_cast<size_t>(index)], nullptr);
    if (display == EGL_NO_DISPLAY) continue;

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) continue;
    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
      eglTerminate(display);
      continue;
    }

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 6,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    EGLContext context =
        eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
      eglTerminate(display);
      continue;
    }
    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
      eglDestroyContext(display, context);
      eglTerminate(display);
      continue;
    }

    renderer_ = glString(GL_RENDERER);
    vendor_ = glString(GL_VENDOR);
    glVersion_ = glString(GL_VERSION);
    if (renderer_.empty()) {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(display, context);
      eglTerminate(display);
      continue;
    }

    display_ = display;
    context_ = context;
    return true;
  }

  error = "no EGL device produced a working OpenGL 4.6 core context";
  return false;
}

bool EglDevice::makeCurrent() const {
  if (context_ == nullptr) return false;
  return eglMakeCurrent(static_cast<EGLDisplay>(display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                        static_cast<EGLContext>(context_)) == EGL_TRUE;
}

void EglDevice::shutdown() {
  if (display_ == nullptr) return;
  EGLDisplay display = static_cast<EGLDisplay>(display_);
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (context_ != nullptr) eglDestroyContext(display, static_cast<EGLContext>(context_));
  eglTerminate(display);
  display_ = nullptr;
  context_ = nullptr;
}

}  // namespace nova::gfx
