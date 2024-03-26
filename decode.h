#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <pthread.h>
#include <string_view>
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

// TODO possibly rename this for generalized libavcodec errors
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

// Returns index of video stream, or -1 if it was not found.
// TODO replace this with av_find_best_stream() or whatever it's
// called.

// maybe add ctrl+C interrupt that just stops and flushes all packets so far?

constexpr size_t CHUNK_FRAME_SIZE = 60;
constexpr size_t NUM_WORKERS = 4;
constexpr size_t THREADS_PER_WORKER = 2;

constexpr size_t framebuf_size = CHUNK_FRAME_SIZE * NUM_WORKERS;
using FrameBuf = std::array<AVFrame*, framebuf_size>;

// yeah so even if you override the destructor, the other destructors
// still run afterward which is good.
struct DemuxerContext {
    // TODO does overriding the destructor make other destructors not run?

    // is there a faster way to delete this constructors?
    DemuxerContext() = delete;
    DemuxerContext(DemuxerContext&& source) = delete;
    DemuxerContext(DemuxerContext&) = delete;
    DemuxerContext& operator=(const DemuxerContext&) = delete;
    DemuxerContext& operator=(const DemuxerContext&&) = delete;

    // TODO make sure this has no overhead
    std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ptr) {
                        avformat_close_input(&ptr);
                    })>
        demuxer;
    std::unique_ptr<AVPacket,
                    decltype([](AVPacket* ptr) { av_packet_free(&ptr); })>
        pkt;

    ~DemuxerContext() = default;
};

struct DecodeContext {
    // these fields can be null
    AVFormatContext* demuxer{nullptr};
    AVPacket* pkt{nullptr};

    AVCodecContext* decoder{nullptr};

    FrameBuf framebuf{};

    // -1 if not initialized; else the index of the video stream
    int video_index = -1;

    // gets the index of the video stream.
    // Populates it if not available.
    // TODO error handling
    // If it returns -1, that means there was no video stream available.
    // TODO should figure out how to make wrapper class where
    // positive values indicates whatever, and negative values indicate
    // error. Ideally with zero overhead. Because variant will take up
    // many more bytes. Shouldn't be used for this kinda stuff.

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
        avformat_close_input(&demuxer);
        avcodec_free_context(&decoder);
    }

    // TODO maybe put another abstraction of just encapsulating
    // format context and wrap that one inside here
    DecodeContext(AVFormatContext* demuxer_, AVCodecContext* decoder_,
                  AVPacket* pkt_, FrameBuf frame_, int vindex)
        : demuxer(demuxer_), pkt(pkt_), decoder(decoder_), framebuf(frame_),
          video_index(vindex) {}

    // Open file and initialize video decoder.
    [[nodiscard]] static std::variant<DecodeContext, DecoderCreationError>
    open(const char* url);
};

// how do you make a static allocation?

// Move cursor up and erase line

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

// returns number of frames decoded
[[nodiscard]] inline int decode_loop(DecodeContext& dc) {

    int framecount = 0;
    while (true) {
        int ret = run_decoder(dc, 0, 1);
        if (ret <= 0) {
            break;
        }
        framecount += ret;
    }

    return framecount;
}

struct CountFramesResult {
    // TODO we need better signaling of this and whatnot
    // Probably make a struct where we have a separate
    // CountFramesResult and another thingy which is
    // the more detailed error information or whatever.

    // TODO check if order of this shit matters cuz I could kinda optimized
    // based on what we need to most probably access

    bool error_occurred : 1;
    unsigned int nb_discarded : 30;
    unsigned int frame_count : 31;
};

[[nodiscard]] CountFramesResult count_video_packets(DecodeContext& dc);
