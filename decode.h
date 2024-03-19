#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <pthread.h>
#include <unistd.h>
#include <variant>

extern "C" {

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

template <typename T, auto Alloc, auto Free> auto make_resource() {
    return std::unique_ptr<T, decltype([](T* ptr) { Free(&ptr); })>(Alloc());
}

struct DecoderCreationError {
    enum DCErrorType : uint8_t {
        AllocationFailure,
        NoVideoStream,
        NoDecoderAvailable,
        AVError
    } type;
    int averror = 0;

    [[nodiscard]] constexpr std::string_view errmsg() const {
        static constexpr std::string_view errmsg_sv[] = {
            [AllocationFailure] = "Allocation Failure in decoder construction",
            [NoVideoStream] = "No video stream exists in input file",
            [NoDecoderAvailable] = "No decoder available for codec",
            [AVError] = "Unspecified AVError occurred",
        };

        return errmsg_sv[this->type];
    }
};

// maybe add ctrl+C interrupt that just stops and flushes all packets so far?

constexpr size_t CHUNK_FRAME_SIZE = 60;
constexpr size_t NUM_WORKERS = 16;
constexpr size_t THREADS_PER_WORKER = 2;

constexpr size_t framebuf_size = CHUNK_FRAME_SIZE * NUM_WORKERS;
using FrameBuf = std::array<AVFrame*, framebuf_size>;

struct DecodeContext {
    // these fields can be null
    AVFormatContext* demuxer{nullptr};
    AVStream* stream{nullptr};
    AVCodecContext* decoder{nullptr};

    AVPacket* pkt{nullptr};
    FrameBuf framebuf{};

    DecodeContext() = delete;

    // move constructor
    DecodeContext(DecodeContext&& source) = delete;

    // copy constructor
    DecodeContext(DecodeContext&) = delete;

    // copy assignment operator
    DecodeContext& operator=(const DecodeContext&) = delete;
    // move assignment operator
    DecodeContext& operator=(const DecodeContext&&) = delete;

    ~DecodeContext() {
        // Since we deleted all the copy/move constructors,
        // we can do this without handling a "moved from" case.

        for (auto* f : framebuf) {
            av_frame_free(&f);
        }

        av_packet_free(&pkt);
        avcodec_free_context(&decoder);
        avformat_close_input(&demuxer);
    }

    DecodeContext(AVFormatContext* demuxer_, AVStream* stream_,
                  AVCodecContext* decoder_, AVPacket* pkt_, FrameBuf frame_)
        : demuxer(demuxer_), stream(stream_), decoder(decoder_), pkt(pkt_),
          framebuf(frame_) {}

    [[nodiscard]] static std::variant<DecodeContext, DecoderCreationError>
    open(const char* url);
};

// how do you make a static allocation?

// Move cursor up and erase line
#define ERASE_LINE_ANSI "\x1B[1A\x1B[2K" // NOLINT

// Honestly this is kinda messed up.
// I think I should double check that all the ffmpeg errors have a negative
// value and stuff. Or just wrap all this in some kind of enum.

// framebuf_offset is the offset to start decoding into within its internal
// frame buffer.
// max_frames is the maximum number of frames that it should decode.
//
// Return value:
// < 0: an error occurred.
// > 0: Number of frames that were decoded.
//
// TODO: check if you have to copy the attributes to the function definition.
[[nodiscard]] int run_decoder(DecodeContext& dc, size_t framebuf_offset,
                              size_t max_frames);

struct DecodeContextResult {
    // This will just have nullptr fields
    DecodeContext dc;
    // if err.averror was non
    DecoderCreationError err;
};
