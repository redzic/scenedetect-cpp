/*
 * http://ffmpeg.org/doxygen/trunk/index.html
 *
 * Main components
 *
 * Format (Container) - a wrapper, providing sync, metadata and muxing for the
 * streams. Stream - a continuous stream (audio or video) of data over time.
 * Codec - defines how data are enCOded (from Frame to Packet)
 *        and DECoded (from Packet to Frame).
 * Packet - are the data (kind of slices of the stream data) to be decoded as
 * raw frames. Frame - a decoded raw frame (to be encoded or filtered).
 */

#include <fmt/core.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// bigger binary size :(
// might have to make my own fmt library or something
// which doesn't have too much bloat

int main() {
  // struct that holds some data about the container (format)
  // does this have to be freed manually?
  auto fctx = avformat_alloc_context();

  if (avformat_open_input(&fctx, "/Users/yusufredzic/Downloads/river.mp4",
                          nullptr, nullptr)) {
    // nonzero return value means FAILURE
    fmt::print("avformat_open_input() returned failure, aborting...\n");
    return -1;
  }

  fmt::print("Format {}, duration {} us\n", fctx->iformat->long_name,
             fctx->duration);

  // this populates some fields in the context
  // possibly not necessary for all formats
  avformat_find_stream_info(fctx, nullptr);

  fmt::print("number of streams: {}\n", fctx->nb_streams);

  for (size_t i = 0; i < fctx->nb_streams; i++) {
    // codec parameters for current stream
    auto codecpar = fctx->streams[i]->codecpar;
    auto codec = avcodec_find_decoder(codecpar->codec_id);

    fmt::print("{: >8} ", codec->name);
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {

      fmt::print("[Video Codec] Resolution {}x{} px\n", codecpar->width,
                 codecpar->height);
    } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      fmt::print("[Audio Codec] {}Ch, Sample Rate={}hz\n",
                 codecpar->ch_layout.nb_channels, codecpar->sample_rate);
    }
  }

  avformat_free_context(fctx);
}
