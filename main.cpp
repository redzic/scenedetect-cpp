#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

inline void w_stdout(std::string_view sv) {
    write(STDOUT_FILENO, sv.data(), sv.size());
}
inline void w_stderr(std::string_view sv) {
    write(STDERR_FILENO, sv.data(), sv.size());
}

// VideoDecodeContext
struct VidDecCtx {
    // all fields are owned and must be non-null
    AVFormatContext* demuxer;
    AVStream* stream;
    AVCodecContext* decoder;

    AVPacket* pkt;
    AVFrame* frame;

    [[nodiscard]] static std::optional<VidDecCtx> open(const char* url) {
        // any way to reduce this boilerplate? jeez...
        auto pkt = std::unique_ptr<AVPacket, decltype([](AVPacket* pkt) {
                                       av_packet_free(&pkt);
                                   })>(av_packet_alloc());
        auto frame = std::unique_ptr<AVFrame, decltype([](AVFrame* frame) {
                                         av_frame_free(&frame);
                                     })>(av_frame_alloc());
        // this should work with the way smart pointers work right?
        if ((pkt == nullptr) || (frame == nullptr)) {
            return {};
        }

        AVFormatContext* raw_demuxer = nullptr;

        // avformat_open_input automatically frees on failure so we construct
        // the smart pointer AFTER this expression.
        if (avformat_open_input(&raw_demuxer, url, nullptr, nullptr) < 0) {
            // these returns need to, like...
            // destruct the object properly
            return {};
        }

        assert(raw_demuxer != nullptr);
        auto demuxer =
            std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ctx) {
                                avformat_close_input(&ctx);
                            })>(raw_demuxer);

        avformat_find_stream_info(demuxer.get(), nullptr);
        // find stream idx of video stream
        // TODO rewrite this in a less error-prone way.
        int stream_idx = -1;
        for (unsigned int sidx = 0; sidx < demuxer->nb_streams; sidx++) {
            if (demuxer->streams[sidx]->codecpar->codec_type ==
                AVMEDIA_TYPE_VIDEO) {
                stream_idx = static_cast<int>(sidx);
            }
        }
        if (stream_idx < 0) {
            return {};
        }

        // index is stored in AVStream->index
        auto* stream = demuxer->streams[stream_idx];

        const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            return {};
        }

        auto decoder =
            std::unique_ptr<AVCodecContext, decltype([](AVCodecContext* ctx) {
                                avcodec_free_context(&ctx);
                            })>(avcodec_alloc_context3(codec));

        if (avcodec_parameters_to_context(decoder.get(), stream->codecpar) <
            0) {
            return {};
        }

        // set automatic threading
        decoder->thread_count = 0;

        return VidDecCtx(demuxer.release(), stream, decoder.release(),
                         pkt.release(), frame.release());
    }

    // For some seemingly inexplicable reason, having this destructor present
    // seems to cause some extremely weird issues to happen where a nonzero
    // exit code is returned from main. This doesn't happen when deleting the
    // destructor.
    ~VidDecCtx() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&decoder);
        avformat_close_input(&demuxer);
    }

    VidDecCtx(auto demuxer_, auto stream_, auto decoder_, auto pkt_,
              auto frame_)
        : demuxer(demuxer_), stream(stream_), decoder(decoder_), pkt(pkt_),
          frame(frame_) {}
};

// so it seems like you have to call
// unref before you reuse AVPacket or AVFrame

void segvHandler(int s) {
    w_stderr("Segmentation Fault\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
    signal(SIGSEGV, segvHandler);

    if (argc != 2) {
        w_stderr("scenedetect-cpp: invalid number of arguments\n"
                 "   usage: scenedetect-cpp  <video_file>\n");
        return -1;
    }

    // can assume argv[1] is now available
    char* url = argv[1];

    // bro how on earth is the exit code being set to
    // something other than 0???
    auto vdec = VidDecCtx::open(url);
    if (vdec) {
        w_stdout("Decoder object constructed\n");
    } else {
        w_stdout("not present\n");
    }

    return 0;
}
