// Headless OpenGL on the NVIDIA GPU with no X server, no Xvfb and no virtual
// display: EGL_EXT_platform_device enumerates the physical device directly and
// EGL_KHR_surfaceless_context gives a context with no window system at all.
//
// Iridium runs at a tty with no Xorg or Plasma, so this is the only sane path.
// It also means the visualiser cannot be disturbed by, or disturb, a desktop
// session that someone might start later.
#pragma once

#include <cstdint>
#include <string>

namespace nova::gfx {

class EglDevice {
 public:
  EglDevice() = default;
  ~EglDevice();

  EglDevice(const EglDevice&) = delete;
  EglDevice& operator=(const EglDevice&) = delete;

  // `preferredCudaDevice` selects among multiple GPUs by CUDA ordinal; -1 takes
  // the first device that yields a working GL 4.6 core context.
  bool initialise(int preferredCudaDevice, std::string& error);
  void shutdown();

  bool makeCurrent() const;
  bool valid() const { return context_ != nullptr; }

  const std::string& renderer() const { return renderer_; }
  const std::string& vendor() const { return vendor_; }
  const std::string& glVersion() const { return glVersion_; }

 private:
  void* display_ = nullptr;  // EGLDisplay
  void* context_ = nullptr;  // EGLContext
  std::string renderer_;
  std::string vendor_;
  std::string glVersion_;
};

}  // namespace nova::gfx
