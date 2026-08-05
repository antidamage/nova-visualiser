#version 460 core

// Attribute-less fullscreen triangle. Port of `phonoscope_fullscreen_vertex`,
// with the UV flip that GL's bottom-left origin needs relative to Metal's
// top-left. The encoder pass flips again on the way out, so the streamed image
// matches what tvOS renders locally.

out vec2 uv;

void main() {
  vec2 positions[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
  vec2 coordinates[3] = vec2[3](vec2(0, 0), vec2(2, 0), vec2(0, 2));
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
  uv = coordinates[gl_VertexID];
}
