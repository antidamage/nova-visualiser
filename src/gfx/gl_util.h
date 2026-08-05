// Thin OpenGL helpers: program compilation with real error reporting, and RAII
// wrappers for the handful of object types the renderer uses.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nova::gfx {

// Compiles and links a program. Returns 0 and fills `error` on failure; the
// service treats that as fatal rather than rendering a black frame forever.
uint32_t buildProgram(std::string_view vertexSource, std::string_view fragmentSource,
                      std::string& error);
uint32_t buildComputeProgram(std::string_view computeSource, std::string& error);

// Reads back the current GL error queue as text; empty when clean.
std::string drainGlErrors(std::string_view context);

}  // namespace nova::gfx
