#include "gfx/gl_util.h"

#include <epoxy/gl.h>

#include <vector>

namespace nova::gfx {
namespace {

uint32_t compileStage(GLenum stage, std::string_view source, std::string& error) {
  const GLuint shader = glCreateShader(stage);
  const char* text = source.data();
  const GLint length = static_cast<GLint>(source.size());
  glShaderSource(shader, 1, &text, &length);
  glCompileShader(shader);

  GLint status = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_TRUE) return shader;

  GLint logLength = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
  std::vector<char> log(static_cast<size_t>(std::max(logLength, 1)));
  glGetShaderInfoLog(shader, logLength, nullptr, log.data());
  error.assign(log.data());
  glDeleteShader(shader);
  return 0;
}

uint32_t link(std::vector<GLuint> stages, std::string& error) {
  const GLuint program = glCreateProgram();
  for (GLuint stage : stages) glAttachShader(program, stage);
  glLinkProgram(program);
  for (GLuint stage : stages) {
    glDetachShader(program, stage);
    glDeleteShader(stage);
  }

  GLint status = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (status == GL_TRUE) return program;

  GLint logLength = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
  std::vector<char> log(static_cast<size_t>(std::max(logLength, 1)));
  glGetProgramInfoLog(program, logLength, nullptr, log.data());
  error.assign(log.data());
  glDeleteProgram(program);
  return 0;
}

}  // namespace

uint32_t buildProgram(std::string_view vertexSource, std::string_view fragmentSource,
                      std::string& error) {
  const GLuint vertex = compileStage(GL_VERTEX_SHADER, vertexSource, error);
  if (vertex == 0) {
    error = "vertex shader: " + error;
    return 0;
  }
  const GLuint fragment = compileStage(GL_FRAGMENT_SHADER, fragmentSource, error);
  if (fragment == 0) {
    glDeleteShader(vertex);
    error = "fragment shader: " + error;
    return 0;
  }
  return link({vertex, fragment}, error);
}

uint32_t buildComputeProgram(std::string_view computeSource, std::string& error) {
  const GLuint compute = compileStage(GL_COMPUTE_SHADER, computeSource, error);
  if (compute == 0) {
    error = "compute shader: " + error;
    return 0;
  }
  return link({compute}, error);
}

std::string drainGlErrors(std::string_view context) {
  std::string result;
  for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
    if (!result.empty()) result += ", ";
    result += std::string(context) + ": 0x";
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04x", error);
    result += buffer;
  }
  return result;
}

}  // namespace nova::gfx
