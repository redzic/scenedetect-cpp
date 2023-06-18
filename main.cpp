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

#include <cassert>
#include <fmt/core.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

static void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize,
                            const char *filename) {
  FILE *f;
  int i;
  f = fopen(filename, "w");
  // writing the minimal required header for a pgm file format
  // portable graymap format ->
  // https://en.wikipedia.org/wiki/Netpbm_format#PGM_example
  fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);

  // writing line by line
  for (i = 0; i < ysize; i++)
    fwrite(buf + i * wrap, 1, xsize, f);
  fclose(f);
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

  for (size_t stream_idx = 0; stream_idx < fctx->nb_streams; stream_idx++) {
    // codec parameters for current stream
    auto codecpar = fctx->streams[stream_idx]->codecpar;

    // find suitable decoder for the codec parameters
    auto codec = avcodec_find_decoder(codecpar->codec_id);

    fmt::print("{: >6} ", codec->name);

    bool skip_decode = true;
    if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      fmt::println("[Video Codec] Resolution {}x{} px", codecpar->width,
                   codecpar->height);
      skip_decode = false;
    } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      fmt::println("[Audio Codec] {}Ch, Sample Rate={}hz",
                   codecpar->ch_layout.nb_channels, codecpar->sample_rate);
    }

    if (skip_decode) {
      continue;
    }

    fmt::println("\t{}, ID {}, bit_rate {}", codec->long_name, (int)codec->id,
                 codecpar->bit_rate);

    //  AVCodecContext is struct for decode/encode

    // allocate context
    auto codec_ctx = avcodec_alloc_context3(codec);
    // fill codec context with parameters
    avcodec_parameters_to_context(codec_ctx, codecpar);
    // initialize codec context based on AVCodec
    avcodec_open2(codec_ctx, codec, nullptr);

    // read packets from stream and decode into frames
    // but we need to allocate the packets and frames first

    auto packet = av_packet_alloc();
    auto frame = av_frame_alloc();

    size_t frame_idx = 0;

    // it really reads a packet basically
    while (av_read_frame(fctx, packet) >= 0) {
      if (packet->stream_index != stream_idx) {
        continue;
      }

      // send compressed packet to the CodecContext (decoder)
      avcodec_send_packet(codec_ctx, packet);

      // receive raw uncompressed frame
      // So is this guaranteed to work this way?
      // send one packet, receive one frame?

      // if this returns EAGAIN, we need to send more input
      int response = avcodec_receive_frame(codec_ctx, frame);

      if (response == AVERROR(EAGAIN)) {
        continue;
      }

      assert(!response);

      auto filename = fmt::format("{}.pgm", frame_idx);

      // so we are indeed decoding some frames
      // but like some of them aren't decoding properly

      save_gray_frame(frame->data[0], frame->linesize[0], frame->width,
                      frame->height, filename.c_str());

      fmt::print("Frame {} ({}) pts {} dts {} key_frame {}\n",
                 av_get_picture_type_char(frame->pict_type),
                 codec_ctx->frame_num, frame->pts, frame->pkt_dts,
                 frame->key_frame);

      frame_idx++;
    }
  }

  avformat_free_context(fctx);
}
