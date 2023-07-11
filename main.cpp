#include <array>
#include <cassert>
#include <corecrt.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <unistd.h>
#include <utility>
#include <variant>

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

template <typename T, auto Alloc, auto Free> auto make_managed() {
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

using FrameBuf = std::array<AVFrame*, 2>;

struct DecodeContext {
    // these fields can be null
    AVFormatContext* demuxer{nullptr};
    AVStream* stream{nullptr};
    AVCodecContext* decoder{nullptr};

    AVPacket* pkt{nullptr};
    FrameBuf framebuf{};

    constexpr DecodeContext() = default;

    // move constructor
    DecodeContext(DecodeContext&& source) = delete;

    // copy constructor
    DecodeContext(DecodeContext&) = delete;

    // copy assignment operator
    DecodeContext& operator=(const DecodeContext&) = delete;
    // move assignment operator
    DecodeContext& operator=(const DecodeContext&&) = delete;

    constexpr ~DecodeContext() {
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
    open(const char* url) {
        auto pkt = make_managed<AVPacket, av_packet_alloc, av_packet_free>();
        auto frame1 = make_managed<AVFrame, av_frame_alloc, av_frame_free>();
        auto frame2 = make_managed<AVFrame, av_frame_alloc, av_frame_free>();

        // this should work with the way smart pointers work right?
        if ((pkt == nullptr) || (frame1 == nullptr) || (frame2 == nullptr)) {
            return DecoderCreationError{
                .type = DecoderCreationError::AllocationFailure};
        }

        AVFormatContext* raw_demuxer = nullptr;

        // avformat_open_input automatically frees on failure so we construct
        // the smart pointer AFTER this expression.
        {
            int ret = avformat_open_input(&raw_demuxer, url, nullptr, nullptr);
            if (ret < 0) {
                return {DecoderCreationError{
                    .type = DecoderCreationError::AVError, .averror = ret}};
            }
        }

        assert(raw_demuxer != nullptr);
        auto demuxer =
            std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ctx) {
                                avformat_close_input(&ctx);
                            })>(raw_demuxer);

        avformat_find_stream_info(demuxer.get(), nullptr);

        // find stream idx of video stream
        int stream_idx = [](AVFormatContext* demuxer) {
            for (unsigned int stream_idx = 0; stream_idx < demuxer->nb_streams;
                 stream_idx++) {
                if (demuxer->streams[stream_idx]->codecpar->codec_type ==
                    AVMEDIA_TYPE_VIDEO) {
                    return static_cast<int>(stream_idx);
                }
            }
            return -1;
        }(demuxer.get());

        if (stream_idx < 0) {
            return {DecoderCreationError{
                .type = DecoderCreationError::NoVideoStream,
            }};
        }

        // index is stored in AVStream->index
        auto* stream = demuxer->streams[stream_idx];

        const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            return {DecoderCreationError{
                .type = DecoderCreationError::NoDecoderAvailable,
            }};
        }

        auto decoder =
            std::unique_ptr<AVCodecContext, decltype([](AVCodecContext* ctx) {
                                avcodec_free_context(&ctx);
                            })>(avcodec_alloc_context3(codec));

        {
            int ret =
                avcodec_parameters_to_context(decoder.get(), stream->codecpar);
            if (ret < 0) {
                return {DecoderCreationError{
                    .type = DecoderCreationError::AVError, .averror = ret}};
            }
        }

        // set automatic threading
        decoder->thread_count = 0;

        FrameBuf framebuf{frame1.release(), frame2.release()};

        return std::variant<DecodeContext, DecoderCreationError>{
            std::in_place_type<DecodeContext>,
            demuxer.release(),
            stream,
            decoder.release(),
            pkt.release(),
            framebuf};
    }
};

uint32_t calc_frame_sum(const uint8_t* ptr, size_t xsize, size_t ysize,
                        size_t stride) {
    uint32_t sum = 0;
    while (ysize-- != 0) {
        for (size_t i = 0; i < xsize; i++) {
            sum += ptr[i];
        }

        ptr += stride;
    }

    return sum;
}

// how do you make a static allocation?

// assume DecodeContext is not in a moved-from state.
int run_decoder(DecodeContext& dc) {
    // AVCodecContext allocated with alloc context
    // previously was allocated with non-NULL codec,
    // so we can pass NULL here.
    int ret = avcodec_open2(dc.decoder, nullptr, nullptr);
    if (ret < 0) [[unlikely]] {
        return ret;
    }

    // start off with first conceptual frame = 0 index
    int accessor_offset = 0;

    auto get_sum = [](AVFrame* f) {
        return calc_frame_sum(f->data[0], f->width, f->height, f->linesize[0]);
    };

    while (true) {
        // Get packet (compressed data) from demuxer
        ret = av_read_frame(dc.demuxer, dc.pkt);
        // EOF
        if (ret < 0) [[unlikely]] {
            break;
        }

        // skip packets other than the ones we're interested in
        if (dc.pkt->stream_index != dc.stream->index) [[unlikely]] {
            av_packet_unref(dc.pkt);
            continue;
        }

        // Send the compressed data to the decoder
        ret = avcodec_send_packet(dc.decoder, dc.pkt);
        if (ret < 0) [[unlikely]] {
            // Error decoding frame
            av_packet_unref(dc.pkt);
            w_stdout("Error decoding frame!\n");
            return ret;
        } else {
            av_packet_unref(dc.pkt);
        }

        // av_frame_unref() is called automatically by this function
        ret =
            avcodec_receive_frame(dc.decoder, dc.framebuf[1 ^ accessor_offset]);

        if (ret == AVERROR(EAGAIN)) [[unlikely]] {
            // need to send more data
            continue;
        }

        if (ret < 0) [[unlikely]] {
            return ret;
        }

        if (dc.decoder->frame_num > 1) [[likely]] {
            // use adjacent pair of frames

            auto s1 = get_sum(dc.framebuf[0 ^ accessor_offset]);
            auto s2 = get_sum(dc.framebuf[1 ^ accessor_offset]);

            printf("Frame Pair: [%d, %d]\n", s1, s2);

            av_frame_unref(dc.framebuf[0 ^ accessor_offset]);
        } else {
            // no unref needed, second frame is already unref
            // and first frame is needed next iteration
        }

        // dc.framebuf[0]->display_picture_number

        // use frame
        // printf("Got frame %lld: %dx%d, stride %d\n", dc.decoder->frame_num,
        //        dc.framebuf[0]->width, dc.framebuf[0]->height,
        //        dc.framebuf[0]->linesize[0]);

        // swap access order of frames
        accessor_offset ^= 1;
    }

    // send flush packet
    avcodec_send_packet(dc.decoder, nullptr);

    // receive last frames
    while (true) {
        // ret = avcodec_receive_frame(dc.decoder, dc.framebuf[0]);

        ret =
            avcodec_receive_frame(dc.decoder, dc.framebuf[1 ^ accessor_offset]);

        // cannot be EAGAIN since we sent everything
        if (ret == AVERROR_EOF) [[unlikely]] {
            return 0;
        } else if (ret < 0) [[unlikely]] {
            return ret;
        }

        // can use frame now
        // printf("Got frame %lld: %dx%d, stride %d\n", dc.decoder->frame_num,
        //        dc.framebuf[0]->width, dc.framebuf[0]->height,
        //        dc.framebuf[0]->linesize[0]);

        if (dc.decoder->frame_num > 1) [[likely]] {
            // use adjacent pair of frames

            auto s1 = get_sum(dc.framebuf[0 ^ accessor_offset]);
            auto s2 = get_sum(dc.framebuf[1 ^ accessor_offset]);

            printf("Frame Pair: [%d, %d]\n", s1, s2);

            av_frame_unref(dc.framebuf[0 ^ accessor_offset]);
        } else {
            // no unref needed, second frame is already unref
            // and first frame is needed next iteration
        }

        accessor_offset ^= 1;
    }

    return 0;
}

struct DecodeContextResult {
    // This will just have nullptr fields
    DecodeContext dc;
    // if err.averror was non
    DecoderCreationError err;
};

// so it seems like you have to call
// unref before you reuse AVPacket or AVFrame

void segvHandler(int /*unused*/) {
    w_stderr("Segmentation Fault\n");
    exit(EXIT_FAILURE); // NOLINT
}

template <class> inline constexpr bool always_false_v = false;

int main(int argc, char** argv) {
    if (signal(SIGSEGV, segvHandler) == SIG_ERR) {
        w_stderr(
            "signal(): failed to set SIGSEGV signal handler, aborting...\n");
        return -1;
    }

    if (argc != 2) {
        w_stderr("scenedetect-cpp: invalid number of arguments\n"
                 "   usage: scenedetect-cpp  <video_file>\n");
        return -1;
    }

    // can assume argv[1] is now available
    char* url = argv[1];

    // bro how on earth is the exit code being set to
    // something other than 0???
    auto vdec = DecodeContext::open(url);

    // so as soon as you use something like std::cout, the binary size increases
    // greatly...

    // so we should probably find a way to not use things that increase the
    // binary size a lot...

    std::visit(
        [](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, DecodeContext>) {
                auto& decoder = arg;

                w_stdout("DecodeContext held in std::variant<>\n");

                int ret = run_decoder(decoder);
                printf("Return value: %d\n", ret);

            } else if constexpr (std::is_same_v<T, DecoderCreationError>) {
                auto error = arg;

                if (error.type == DecoderCreationError::AVError) {
                    std::array<char, AV_ERROR_MAX_STRING_SIZE> errbuf{};
                    av_make_error_string(errbuf.data(), errbuf.size(),
                                         error.averror);

                    (void)fprintf(stderr, "Failed to initialize decoder: %s\n",
                                  errbuf.data());

                } else {
                    std::string_view errmsg = error.errmsg();
                    (void)fprintf(stderr,
                                  "Failed to initialize decoder: %.*s\n",
                                  (int)errmsg.size(), errmsg.data());
                }

            } else {
                static_assert(always_false_v<T>);
            }
        },
        vdec);
}
