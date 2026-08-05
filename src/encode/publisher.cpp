#include "encode/publisher.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace nova::encode {
namespace {

std::string averror(int code, const std::string& what) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buffer, sizeof(buffer));
  return what + ": " + buffer;
}

}  // namespace

RtspPublisher::~RtspPublisher() { close(); }

bool RtspPublisher::open(const std::string& url, const EncoderSettings& settings,
                         const std::vector<uint8_t>& parameterSets, std::string& error) {
  close();
  url_ = url;
  frameRate_ = settings.frameRate;

  int status = avformat_alloc_output_context2(&format_, nullptr, "rtsp", url.c_str());
  if (status < 0 || format_ == nullptr) {
    error = averror(status, "avformat_alloc_output_context2(rtsp)");
    return false;
  }
  // TCP interleaved: this is a loopback hop to a sidecar, so the UDP
  // retransmit/jitter machinery buys nothing and packet loss would be silly.
  av_opt_set(format_->priv_data, "rtsp_transport", "tcp", 0);

  stream_ = avformat_new_stream(format_, nullptr);
  if (stream_ == nullptr) {
    error = "avformat_new_stream failed";
    close();
    return false;
  }
  stream_->time_base = AVRational{1, settings.frameRate};
  AVCodecParameters* parameters = stream_->codecpar;
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = settings.codec == Codec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
  parameters->width = settings.width;
  parameters->height = settings.height;
  parameters->format = settings.tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
  parameters->color_space = AVCOL_SPC_BT709;
  parameters->color_primaries = AVCOL_PRI_BT709;
  parameters->color_trc = AVCOL_TRC_BT709;
  parameters->color_range = AVCOL_RANGE_MPEG;
  if (!parameterSets.empty()) {
    parameters->extradata =
        static_cast<uint8_t*>(av_mallocz(parameterSets.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (parameters->extradata != nullptr) {
      std::copy(parameterSets.begin(), parameterSets.end(), parameters->extradata);
      parameters->extradata_size = static_cast<int>(parameterSets.size());
    }
  }

  status = avformat_write_header(format_, nullptr);
  if (status < 0) {
    error = averror(status, "avformat_write_header (is MediaMTX running?)");
    close();
    return false;
  }

  packet_ = av_packet_alloc();
  return true;
}

bool RtspPublisher::write(const EncodedFrame& frame, std::string& error) {
  if (format_ == nullptr || packet_ == nullptr) {
    error = "publisher is not connected";
    return false;
  }
  av_packet_unref(packet_);
  packet_->data = const_cast<uint8_t*>(frame.data.data());
  packet_->size = static_cast<int>(frame.data.size());
  packet_->pts = frame.pts;
  packet_->dts = frame.pts;
  packet_->duration = 1;
  packet_->stream_index = stream_->index;
  packet_->flags = frame.keyframe ? AV_PKT_FLAG_KEY : 0;

  const int status = av_write_frame(format_, packet_);
  // Detach the borrowed buffer before av_packet_unref could try to free it.
  packet_->data = nullptr;
  packet_->size = 0;
  if (status < 0) {
    error = averror(status, "av_write_frame");
    return false;
  }
  return true;
}

void RtspPublisher::close() {
  if (packet_ != nullptr) {
    packet_->data = nullptr;
    packet_->size = 0;
    av_packet_free(&packet_);
  }
  if (format_ != nullptr) {
    if (format_->pb != nullptr || (format_->oformat->flags & AVFMT_NOFILE) != 0) {
      av_write_trailer(format_);
    }
    avformat_free_context(format_);
    format_ = nullptr;
  }
  stream_ = nullptr;
}

}  // namespace nova::encode
