#include "encode/encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
}

#include <algorithm>

namespace nova::encode {
namespace {

std::string averror(int code, const std::string& what) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buffer, sizeof(buffer));
  return what + ": " + buffer;
}

// Copies the VPS/SPS/PPS NAL units out of an Annex-B access unit, start codes
// included, so the result can be handed to a client verbatim.
std::vector<uint8_t> extractParameterSets(const std::vector<uint8_t>& annexB) {
  std::vector<uint8_t> sets;
  size_t index = 0;
  size_t unitStart = std::string::npos;
  int unitType = -1;

  auto startCodeLength = [&annexB](size_t at) -> size_t {
    if (at + 3 < annexB.size() && annexB[at] == 0 && annexB[at + 1] == 0 && annexB[at + 2] == 0 &&
        annexB[at + 3] == 1) {
      return 4;
    }
    if (at + 2 < annexB.size() && annexB[at] == 0 && annexB[at + 1] == 0 && annexB[at + 2] == 1) {
      return 3;
    }
    return 0;
  };

  auto flush = [&](size_t end) {
    // HEVC NAL types 32/33/34 are VPS/SPS/PPS.
    if (unitStart == std::string::npos) return;
    if (unitType == 32 || unitType == 33 || unitType == 34) {
      sets.insert(sets.end(), annexB.begin() + static_cast<long>(unitStart),
                  annexB.begin() + static_cast<long>(end));
    }
  };

  while (index < annexB.size()) {
    const size_t prefix = startCodeLength(index);
    if (prefix == 0) {
      ++index;
      continue;
    }
    flush(index);
    unitStart = index;
    const size_t payload = index + prefix;
    unitType = payload < annexB.size() ? (annexB[payload] >> 1) & 0x3F : -1;
    index = payload;
  }
  flush(annexB.size());
  return sets;
}

}  // namespace

Encoder::~Encoder() { shutdown(); }

bool Encoder::initialise(CUcontext cudaContext, const EncoderSettings& settings,
                         std::string& error) {
  settings_ = settings;

  const char* codecName = settings.codec == Codec::Hevc ? "hevc_nvenc" : "h264_nvenc";
  const AVCodec* codec = avcodec_find_encoder_by_name(codecName);
  if (codec == nullptr) {
    error = std::string("encoder not available: ") + codecName;
    return false;
  }

  deviceRef_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
  if (deviceRef_ == nullptr) {
    error = "av_hwdevice_ctx_alloc failed";
    return false;
  }
  // Adopt the renderer's CUDA context rather than creating a second one, so the
  // encoder and the GL interop share a single context on a single device.
  auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(deviceRef_->data);
  static_cast<AVCUDADeviceContext*>(deviceContext->hwctx)->cuda_ctx = cudaContext;
  int status = av_hwdevice_ctx_init(deviceRef_);
  if (status < 0) {
    error = averror(status, "av_hwdevice_ctx_init");
    return false;
  }

  framesRef_ = av_hwframe_ctx_alloc(deviceRef_);
  if (framesRef_ == nullptr) {
    error = "av_hwframe_ctx_alloc failed";
    return false;
  }
  auto* framesContext = reinterpret_cast<AVHWFramesContext*>(framesRef_->data);
  framesContext->format = AV_PIX_FMT_CUDA;
  framesContext->sw_format = settings.tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
  framesContext->width = settings.width;
  framesContext->height = settings.height;
  // The renderer reuses one input surface, so a small pool is enough.
  framesContext->initial_pool_size = 4;
  status = av_hwframe_ctx_init(framesRef_);
  if (status < 0) {
    error = averror(status, "av_hwframe_ctx_init");
    return false;
  }

  context_ = avcodec_alloc_context3(codec);
  context_->width = settings.width;
  context_->height = settings.height;
  context_->pix_fmt = AV_PIX_FMT_CUDA;
  context_->sw_pix_fmt = framesContext->sw_format;
  context_->time_base = AVRational{1, settings.frameRate};
  context_->framerate = AVRational{settings.frameRate, 1};
  context_->bit_rate = settings.bitRate;
  context_->rc_max_rate = settings.bitRate;
  context_->rc_buffer_size = static_cast<int>(settings.bitRate / settings.frameRate * 2);
  context_->gop_size =
      std::max(1, static_cast<int>(settings.keyframeIntervalSeconds * settings.frameRate));
  // B-frames add a reorder delay the visualiser cannot afford: the whole point
  // of the bespoke transport is that a beat reaches the screen promptly.
  context_->max_b_frames = 0;
  // Signal BT.709 explicitly. Without this the Apple TV guesses, and the
  // streamed colours drift away from what the dashboard shows.
  context_->colorspace = AVCOL_SPC_BT709;
  context_->color_primaries = AVCOL_PRI_BT709;
  context_->color_trc = AVCOL_TRC_BT709;
  context_->color_range = AVCOL_RANGE_MPEG;
  context_->hw_frames_ctx = av_buffer_ref(framesRef_);
  if (settings.globalHeaderParameterSets) {
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  av_opt_set(context_->priv_data, "preset", "p4", 0);
  av_opt_set(context_->priv_data, "tune", "ull", 0);
  av_opt_set(context_->priv_data, "rc", "cbr", 0);
  av_opt_set_int(context_->priv_data, "delay", 0, 0);
  av_opt_set_int(context_->priv_data, "zerolatency", 1, 0);
  av_opt_set_int(context_->priv_data, "repeat_spspps", 1, 0);
  // Without this, NVENC answers a forced I-frame with a plain I-frame, which is
  // NOT a stream access point: a client waiting for a keyframe after a dropout
  // keeps waiting, because a non-IDR I-frame can still reference pictures the
  // client never received. `requestKeyframe()` is only honest with it set.
  av_opt_set_int(context_->priv_data, "forced-idr", 1, 0);
  if (settings.codec == Codec::Hevc) {
    av_opt_set(context_->priv_data, "profile", settings.tenBit ? "main10" : "main", 0);
  } else {
    // Browsers decode H.264 High 8-bit; High 10 is not supported anywhere useful.
    av_opt_set(context_->priv_data, "profile", "high", 0);
  }

  status = avcodec_open2(context_, codec, nullptr);
  if (status < 0) {
    error = averror(status, std::string("avcodec_open2(") + codecName + ")");
    return false;
  }

  if (context_->extradata != nullptr && context_->extradata_size > 0) {
    parameterSets_.assign(context_->extradata,
                          context_->extradata + context_->extradata_size);
  }

  frame_ = av_frame_alloc();
  frame_->format = AV_PIX_FMT_CUDA;
  frame_->width = settings.width;
  frame_->height = settings.height;
  status = av_hwframe_get_buffer(framesRef_, frame_, 0);
  if (status < 0) {
    error = averror(status, "av_hwframe_get_buffer");
    return false;
  }
  packet_ = av_packet_alloc();
  return true;
}

int Encoder::lumaStrideBytes() const { return frame_ != nullptr ? frame_->linesize[0] : 0; }
int Encoder::chromaStrideBytes() const { return frame_ != nullptr ? frame_->linesize[1] : 0; }
void* Encoder::lumaPlane() const { return frame_ != nullptr ? frame_->data[0] : nullptr; }
void* Encoder::chromaPlane() const { return frame_ != nullptr ? frame_->data[1] : nullptr; }

bool Encoder::encode(int64_t pts, std::vector<EncodedFrame>& out, std::string& error) {
  if (context_ == nullptr) {
    error = "encoder is not initialised";
    return false;
  }

  frame_->pts = pts;
  frame_->pict_type = keyframeRequested_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
  if (keyframeRequested_) {
    frame_->flags |= AV_FRAME_FLAG_KEY;
    keyframeRequested_ = false;
  } else {
    frame_->flags &= ~AV_FRAME_FLAG_KEY;
  }

  int status = avcodec_send_frame(context_, frame_);
  if (status < 0) {
    error = averror(status, "avcodec_send_frame");
    return false;
  }

  while (true) {
    status = avcodec_receive_packet(context_, packet_);
    if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
    if (status < 0) {
      error = averror(status, "avcodec_receive_packet");
      return false;
    }
    EncodedFrame encoded;
    encoded.data.assign(packet_->data, packet_->data + packet_->size);
    encoded.pts = packet_->pts;
    encoded.keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
    // With in-band parameter sets there is no extradata, so the first IDR is
    // where the handshake's copy comes from. Clients rebuild from every IDR
    // anyway; this just gives a joining client a running start.
    if (encoded.keyframe && parameterSets_.empty()) {
      parameterSets_ = extractParameterSets(encoded.data);
    }
    out.push_back(std::move(encoded));
    av_packet_unref(packet_);
  }
  return true;
}

void Encoder::shutdown() {
  if (packet_ != nullptr) av_packet_free(&packet_);
  if (frame_ != nullptr) av_frame_free(&frame_);
  if (context_ != nullptr) avcodec_free_context(&context_);
  if (framesRef_ != nullptr) av_buffer_unref(&framesRef_);
  if (deviceRef_ != nullptr) av_buffer_unref(&deviceRef_);
  parameterSets_.clear();
}

}  // namespace nova::encode
