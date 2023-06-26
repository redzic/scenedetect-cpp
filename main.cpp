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

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <thread>
#include <unistd.h>

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

using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;

#define ForceInline __attribute__((always_inline)) inline
#define NoInline __attribute__((noinline))

namespace {

ForceInline void svprint(std::string_view strv) {
    write(STDOUT_FILENO, strv.data(), strv.size());
}

void save_gray_frame(const uint8* __restrict buf, size_t stride, size_t xsize,
                     size_t ysize, const char* filename) {
    FILE* file = fopen(filename, "w");
    // writing the minimal required header for a pgm file format
    // portable graymap format ->
    // https://en.wikipedia.org/wiki/Netpbm_format#PGM_example

    assert(fprintf(file, "P5\n%zu %zu\n255\n", xsize, ysize) > 0);

    if (stride == xsize) {
        auto filed = fileno(file);
        auto n_written = write(filed, buf, xsize * ysize);
        assert((n_written > 0) && ((size_t)n_written == (xsize * ysize)));
    } else {
        // writing line by line
        for (size_t i = 0; i < ysize; i++) {
            assert(fwrite(buf, 1, xsize, file) == xsize);
            buf += stride;
        }
    }

    assert(!fclose(file));
}

// assumes each source plane has the same dimensions
uint32 sum_abs_diff(const uint8* src1, const uint8* src2, size_t stride,
                    size_t width, size_t height) {
    constexpr auto absdiff = [](uint8 a, uint8 b) {
        return std::abs(static_cast<int32>(a) - static_cast<int32>(b));
    };

    uint32 sum = 0;

    if (stride == width) {
        for (size_t i = 0; i < (width * height); i++) {
            sum += absdiff(src1[i], src2[i]);
        }
    } else {
        while ((height--) != 0) {
            for (size_t i = 0; i < width; i++) {
                sum += absdiff(src1[i], src2[i]);
            }

            src1 += stride;
            src2 += stride;
        }
    }

    return sum;
}

//

// bigger binary size :(
// might have to make my own fmt library or something
// which doesn't have too much bloat

// printf that compiles to like "printf bytecode" or something would be
// probably the most ideal. That way it's fast and has minimal calling overhead.
// then you would run "interpreter" of bytecode.
// and has possibility to inline prints of just a string to
// write().

} // namespace

int main(int argc, const char* argv[]) {
    // struct that holds some data about the container (format)
    // does this have to be freed manually?

    if (argc < 2) {
        svprint("scenedetect-cpp requires an input file to be specified, "
                "aborting...\n");
        return -1;
    }

    auto* fctx = avformat_alloc_context();

    if (avformat_open_input(&fctx, argv[1], nullptr, nullptr) != 0) {
        // nonzero return value means FAILURE
        svprint("avformat_open_input() returned failure, aborting...\n");
        return -1;
    }

    printf("Format %s, duration %lld us\n", fctx->iformat->long_name,
           fctx->duration);

    // this populates some fields in the context
    // possibly not necessary for all formats
    avformat_find_stream_info(fctx, nullptr);

    printf("number of streams: %d\n", fctx->nb_streams);

    uint64 time_ns = 0;

    for (size_t stream_idx = 0; stream_idx < fctx->nb_streams; stream_idx++) {
        // codec parameters for current stream
        auto* codecpar = fctx->streams[stream_idx]->codecpar;

        // find suitable decoder for the codec parameters
        const auto* codec = avcodec_find_decoder(codecpar->codec_id);

        // fmt::print("{: >6} ", codec->name);
        printf("%s ", codec->name);

        bool skip_decode = true;
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            printf("[Video Codec] Resolution %dx%d px\n", codecpar->width,
                   codecpar->height);

            skip_decode = false;
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            printf("[Audio Codec] %dCh, Sample Rate=%dhz\n",
                   codecpar->ch_layout.nb_channels, codecpar->sample_rate);
        }

        if (skip_decode) {
            continue;
        }

        printf("\t%s, ID %d, bit_rate %lld\n", codec->long_name, (int)codec->id,
               codecpar->bit_rate);

        //  AVCodecContext is struct for decode/encode

        // allocate context
        auto* codec_ctx = avcodec_alloc_context3(codec);

        // yay! this actually works as intended

        auto n_threads = std::thread::hardware_concurrency();
        if (n_threads > 0) {
            codec_ctx->thread_count = static_cast<int32>(n_threads);
        }

        // fill codec context with parameters
        avcodec_parameters_to_context(codec_ctx, codecpar);
        // initialize codec context based on AVCodec
        avcodec_open2(codec_ctx, codec, nullptr);

        // read packets from stream and decode into frames
        // but we need to allocate the packets and frames first

        std::array<AVFrame*, 2> framebuf{av_frame_alloc(), av_frame_alloc()};

        auto* packet = av_packet_alloc();

        // it really reads a packet basically
        while (av_read_frame(fctx, packet) >= 0) {
            if (packet->stream_index != (int)stream_idx) {
                continue;
            }

            // send compressed packet to the CodecContext (decoder)
            avcodec_send_packet(codec_ctx, packet);

            // receive raw uncompressed frame
            // So is this guaranteed to work this way?
            // send one packet, receive one frame?

            std::swap(framebuf[0], framebuf[1]);

            // if this returns EAGAIN, we need to send more input

            // here we receive uncompressed frame

            // framebuf contains previous 2 frames
            // discard old one

            int response = avcodec_receive_frame(codec_ctx, framebuf[1]);

            if (response == AVERROR(EAGAIN)) {
                continue;
            }

            assert(!response);

            // so we are indeed decoding some frames
            // but like some of them aren't decoding properly

            // save_gray_frame(frame->data[0], frame->linesize[0], frame->width,
            //                 frame->height, filename.c_str());

            AVFrame* frame = framebuf[1];

            // TODO set build options in such a way that this
            // function call gets inlined?

            printf("Frame %c (%lld) pts %lld dts %lld key_frame %d\n",
                   av_get_picture_type_char(frame->pict_type),
                   codec_ctx->frame_num, frame->pts, frame->pkt_dts,
                   frame->key_frame);

            if (codec_ctx->frame_num > 1) {
                // TODO assert all these strides and stuff are the same for
                // both
                auto get_time = []() {
                    return std::chrono::high_resolution_clock::now();
                };
                auto start = get_time();

                // Idea: average pixels in 8x8 blocks to avoid false
                // positives due to noise in pixels

                auto diff =
                    sum_abs_diff(framebuf[0]->data[0], framebuf[1]->data[0],
                                 framebuf[0]->linesize[0], framebuf[0]->width,
                                 framebuf[0]->height);

                auto end = get_time();

                auto duration =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end -
                                                                         start);

                // TODO add assert for checking if max abs diff will fit in
                // uint32

                time_ns += duration.count();
                printf("abs_diff(): %d\n\n", diff);
            }
        }
    }

    avformat_free_context(fctx);

    printf("Total time consumed: %lluns\n", time_ns);

    return 0;
}
