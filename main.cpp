#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

namespace {

#define AlwaysInline __attribute__((always_inline)) inline

AlwaysInline void w_stdout(std::string_view sv) {
    write(STDOUT_FILENO, sv.data(), sv.size());
}
AlwaysInline void w_stderr(std::string_view sv) {
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

// Assumes same dimensions between frames
uint32_t calc_frame_sad(const uint8_t* __restrict ptr1,
                        const uint8_t* __restrict ptr2, size_t xsize,
                        size_t ysize, size_t stride) {
    uint32_t sum = 0;
    while (ysize-- != 0) {
        for (size_t i = 0; i < xsize; i++) {
            sum += std::abs(static_cast<int32_t>(ptr1[i]) -
                            static_cast<int32_t>(ptr2[i]));
        }

        ptr1 += stride;
        ptr2 += stride;
    }

    return sum;
}

// how do you make a static allocation?

// Move cursor up and erase line
#define ERASE_LINE_ANSI "\x1B[1A\x1B[2K"

// assume DecodeContext is not in a moved-from state.
int run_decoder(DecodeContext& dc, std::vector<uint32_t>* scores) {
    // AVCodecContext allocated with alloc context
    // previously was allocated with non-NULL codec,
    // so we can pass NULL here.
    int ret = avcodec_open2(dc.decoder, nullptr, nullptr);
    if (ret < 0) [[unlikely]] {
        return ret;
    }

    // start off with first conceptual frame = 0 index
    int accessor_offset = 0;

    auto get_sad = [](AVFrame* f1, AVFrame* f2) {
        assert(f1->width == f2->width);
        assert(f1->height == f2->height);
        assert(f1->linesize[0] == f2->linesize[0]);

        return calc_frame_sad(f1->data[0], f2->data[0], f1->width, f1->height,
                              f1->linesize[0]);
    };

    int last_frame = 0;

    auto receive_frames = [&dc, accessor_offset, &get_sad, scores]() mutable {
        // receive last frames
        while (true) {
            int ret = avcodec_receive_frame(dc.decoder,
                                            dc.framebuf[1 ^ accessor_offset]);

            if (ret < 0) [[unlikely]] {
                return ret;
            }

            if (dc.decoder->frame_num > 1) [[likely]] {
                // use adjacent pair of frames

                auto s1 = get_sad(dc.framebuf[0 ^ accessor_offset],
                                  dc.framebuf[1 ^ accessor_offset]);

                scores->push_back(s1);
                // printf("Frame Pair SAD: %d\n", s1);

                av_frame_unref(dc.framebuf[0 ^ accessor_offset]);
            } else {
                // no unref needed, second frame is already unref
                // and first frame is needed next iteration
            }

            accessor_offset ^= 1;
        }
    };

    printf("Received 0 frames so far\n");

    while (true) {
        // Get packet (compressed data) from demuxer
        int ret = av_read_frame(dc.demuxer, dc.pkt);
        // EOF in compressed data
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

            w_stdout("Error decoding frame!\nError was not EAGAIN\n");

            return ret;
        } else {
            av_packet_unref(dc.pkt);
        }

        receive_frames();

        if (dc.decoder->frame_num - last_frame > 256) [[unlikely]] {
            last_frame = (int)dc.decoder->frame_num;

            printf(ERASE_LINE_ANSI "Received %d frames so far\n", last_frame);
        }
    }

    // send flush packet
    avcodec_send_packet(dc.decoder, nullptr);
    receive_frames();

    printf(ERASE_LINE_ANSI "Received %d frames so far\n",
           (int)dc.decoder->frame_num);

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

template <class result_t = std::chrono::milliseconds,
          class clock_t = std::chrono::steady_clock,
          class duration_t = std::chrono::milliseconds>
auto since(std::chrono::time_point<clock_t, duration_t> const& start) {
    return std::chrono::duration_cast<result_t>(clock_t::now() - start);
}

auto now() { return std::chrono::steady_clock::now(); }

} // namespace

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
                auto& d_ctx = arg;

                w_stdout("DecodeContext held in std::variant<>\n");

                std::vector<uint32_t> scores;
                scores.reserve(1024);

                auto start = now();
                int ret = run_decoder(d_ctx, &scores);
                auto elapsed_ms = since(start).count();

                auto frames = d_ctx.decoder->frame_num;

                if (ret == 0) {
                    double fps = 1000.0 * (static_cast<double>(frames) /
                                           static_cast<double>(elapsed_ms));

                    printf(
                        "Successfully decoded %d frames in %lld ms (%f fps)\n",
                        (int)frames, elapsed_ms, fps);

                    printf("Scores len: %llu\n", scores.size());
                    printf("Writing data to scores.txt\n");

                    std::ofstream o_file;
                    o_file.open("scores.txt");
                    for (auto score : scores) {
                        o_file << score << '\n';
                    }
                    o_file.close();

                } else {
                    printf("Decoding error! value: %d\n", ret);
                }

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
