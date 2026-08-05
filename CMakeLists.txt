cmake_minimum_required(VERSION 3.22)
project(nova_visualiser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

find_package(Threads REQUIRED)

# The GPU half is optional so the engine core and the conformance runner build
# and run on any machine -- including one with no NVIDIA driver, which is how
# the corpus stays easy to check during module work.
option(NOVA_VISUALISER_GPU "Build the GPU renderer, encoder and service" ON)

if(NOVA_VISUALISER_GPU)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)
  pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
  # FreeType rasterises the centre message. Required rather than optional: the
  # message is part of the streamed picture now, and silently dropping it would
  # be a parity regression against tvOS rather than a degraded transport.
  pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
  # SRT carries the Apple TV rung. Optional so a box without libsrt still builds
  # and serves the TCP rung -- which is also the runtime fallback, so a missing
  # library degrades the transport rather than the service.
  pkg_check_modules(SRT IMPORTED_TARGET srt)
  if(SRT_FOUND)
    message(STATUS "libsrt found: building the SRT transport rung")
  else()
    message(WARNING "libsrt not found: SRT rung disabled, Apple TV falls back to TCP. "
                    "Install libsrt-openssl-dev to enable it.")
  endif()

  find_path(CUDA_DRIVER_INCLUDE_DIR cuda.h PATHS /usr/include /usr/local/cuda/include)
  find_library(CUDA_DRIVER_LIBRARY NAMES cuda PATHS /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)
  if(NOT CUDA_DRIVER_INCLUDE_DIR OR NOT CUDA_DRIVER_LIBRARY)
    message(FATAL_ERROR "CUDA driver API (cuda.h / libcuda) not found")
  endif()
endif()

# ---------------------------------------------------------------------------
# nova_visualiser_core -- engine with no GL/CUDA/network dependency. This is the
# half that must stay in lockstep with the tvOS Metal engine, so it is a
# separate library that the conformance runner links without a GPU present.
# ---------------------------------------------------------------------------
add_library(nova_visualiser_core STATIC
  src/core/expression.cpp
  src/core/json.cpp
  src/core/module.cpp
  src/core/palette.cpp
  src/core/simulation.cpp
  src/core/signal.cpp
)
target_include_directories(nova_visualiser_core PUBLIC src)
target_link_libraries(nova_visualiser_core PUBLIC Threads::Threads)

if(NOVA_VISUALISER_GPU)
  # ---------------------------------------------------------------------------
  # nova-visualiser -- the service
  # ---------------------------------------------------------------------------
  add_executable(nova-visualiser
    src/service/main.cpp
    src/service/options.cpp
    src/service/engine.cpp
    src/gfx/egl_device.cpp
    src/gfx/gl_util.cpp
    src/gfx/renderer.cpp
    src/gfx/text.cpp
    src/encode/cuda_interop.cpp
    src/encode/encoder.cpp
    src/encode/publisher.cpp
    src/net/http_client.cpp
    src/net/sse_client.cpp
    src/net/config_client.cpp
    src/net/stream_server.cpp
    src/net/control_server.cpp
    src/net/house_party.cpp
  )
  target_include_directories(nova-visualiser PRIVATE src ${CUDA_DRIVER_INCLUDE_DIR})
  target_link_libraries(nova-visualiser PRIVATE
    nova_visualiser_core
    PkgConfig::AVCODEC
    PkgConfig::EPOXY
    PkgConfig::FREETYPE
    ${CUDA_DRIVER_LIBRARY}
    EGL
    Threads::Threads
  )
  if(SRT_FOUND)
    target_sources(nova-visualiser PRIVATE src/net/srt_server.cpp)
    target_link_libraries(nova-visualiser PRIVATE PkgConfig::SRT)
    target_compile_definitions(nova-visualiser PRIVATE NOVA_VISUALISER_HAVE_SRT=1)
  endif()

  # Shaders are embedded at build time so the installed service has no runtime
  # asset lookup and cannot half-start with a missing file.
  set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders)
  file(GLOB SHADER_FILES ${SHADER_DIR}/*.vert ${SHADER_DIR}/*.frag ${SHADER_DIR}/*.comp)
  set(SHADER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders.h)
  add_custom_command(
    OUTPUT ${SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND}
            -DSHADER_DIR=${SHADER_DIR}
            -DOUTPUT=${SHADER_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    DEPENDS ${SHADER_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    COMMENT "Embedding GLSL shaders"
  )
  add_custom_target(nova_visualiser_shaders DEPENDS ${SHADER_HEADER})
  add_dependencies(nova-visualiser nova_visualiser_shaders)
  target_include_directories(nova-visualiser PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

  # Validate every shader at build time; a GLSL syntax error must not reach the
  # GPU as a runtime failure on the box that runs the house.
  find_program(GLSLANG_VALIDATOR glslangValidator)
  if(GLSLANG_VALIDATOR)
    add_custom_command(TARGET nova_visualiser_shaders POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo "Validating GLSL"
      COMMAND ${GLSLANG_VALIDATOR} ${SHADER_FILES}
    )
  endif()

  # ---------------------------------------------------------------------------
  # Probes and tests
  # ---------------------------------------------------------------------------
  add_executable(nova-visualiser-probe src/tools/probe.cpp src/gfx/egl_device.cpp src/gfx/gl_util.cpp)
  target_include_directories(nova-visualiser-probe PRIVATE src ${CUDA_DRIVER_INCLUDE_DIR})
  target_link_libraries(nova-visualiser-probe PRIVATE
    PkgConfig::AVCODEC PkgConfig::EPOXY ${CUDA_DRIVER_LIBRARY} EGL Threads::Threads)
endif()

add_executable(nova-visualiser-conformance src/tools/conformance.cpp)
target_include_directories(nova-visualiser-conformance PRIVATE src)
target_link_libraries(nova-visualiser-conformance PRIVATE nova_visualiser_core)

enable_testing()
add_test(NAME conformance
         COMMAND nova-visualiser-conformance --corpus ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance)

if(NOVA_VISUALISER_GPU)
  install(TARGETS nova-visualiser RUNTIME DESTINATION bin)
endif()
