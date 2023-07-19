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
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
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

constexpr size_t framebuf_size = 512;
using FrameBuf = std::array<AVFrame*, framebuf_size>;

struct DecodeContext {
    // these fields can be null
    AVFormatContext* demuxer{nullptr};
    AVStream* stream{nullptr};
    AVCodecContext* decoder{nullptr};

    AVPacket* pkt{nullptr};
    FrameBuf framebuf{};
    int framebuf_output_index = 0;

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

        printf("~DecodeContext()\n");

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

        // this should work with the way smart pointers work right?
        if (pkt == nullptr) {
            return DecoderCreationError{
                .type = DecoderCreationError::AllocationFailure};
        }

        // auto frame1 = make_managed<AVFrame, av_frame_alloc, av_frame_free>();

        // TODO properly clean up resources on alloc failure
        FrameBuf frame_buffer{};

        for (auto& frame : frame_buffer) {
            frame = av_frame_alloc();
            if (frame == nullptr) {
                return DecoderCreationError{
                    .type = DecoderCreationError::AllocationFailure};
            }
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

        return std::variant<DecodeContext, DecoderCreationError>{
            std::in_place_type<DecodeContext>,
            demuxer.release(),
            stream,
            decoder.release(),
            pkt.release(),
            frame_buffer};
    }
};

// Assumes same dimensions between frames
// uint32_t calc_frame_sad(const uint8_t* __restrict ptr1,
//                         const uint8_t* __restrict ptr2, size_t xsize,
//                         size_t ysize, size_t stride) {
//     uint32_t sum = 0;
//     while (ysize-- != 0) {
//         for (size_t i = 0; i < xsize; i++) {
//             sum += std::abs(static_cast<int32_t>(ptr1[i]) -
//                             static_cast<int32_t>(ptr2[i]));
//         }

//         ptr1 += stride;
//         ptr2 += stride;
//     }

//     return sum;
// }

// how do you make a static allocation?

// Move cursor up and erase line
#define ERASE_LINE_ANSI "\x1B[1A\x1B[2K" // NOLINT

// assume DecodeContext is not in a moved-from state.
int run_decoder(DecodeContext& dc) {
    // AVCodecContext allocated with alloc context
    // previously was allocated with non-NULL codec,
    // so we can pass NULL here.
    int ret = avcodec_open2(dc.decoder, nullptr, nullptr);
    if (ret < 0) [[unlikely]] {
        return ret;
    }

    // int last_frame = 0;

    auto receive_frames = [&dc]() {
        // receive last frames
        while (dc.framebuf_output_index < (int)framebuf_size) {
            int ret = avcodec_receive_frame(
                dc.decoder, dc.framebuf[dc.framebuf_output_index]);
            if (ret < 0) [[unlikely]] {
                return ret;
            }

            dc.framebuf_output_index += 1;
        }
        return 0;
    };

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

            w_stdout("Error decoding frame!\n");

            return ret;
        } else {
            av_packet_unref(dc.pkt);
        }

        // receive as many frames as possible up until max size
        receive_frames();
        if (dc.framebuf_output_index >= (int)framebuf_size) {
            return 0;
        }
    }

    // once control flow reached here, it is guaranteed that more frames need to
    // be received to fill the buffer send flush packet
    avcodec_send_packet(dc.decoder, nullptr);
    receive_frames();

    printf("Received %d frames so far\n", (int)dc.decoder->frame_num);

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

int encode(AVCodecContext* enc_ctx, AVFrame* frame, AVPacket* pkt,
           FILE* ostream) {
    // frame can be null, which is considered a flush frame
    int ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) {
        printf("error sending frame to encoder\n");
        return ret;
    }

    while (true) {
        int ret = avcodec_receive_packet(enc_ctx, pkt);
        // why check for eof though?
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            printf("unspecified error during encoding\n");
            return ret;
        }

        // can write the compressed packet to the bitstream now

        // ostream.write(reinterpret_cast<char*>(pkt->data), pkt->size);
        (void)fwrite(pkt->data, pkt->size, 1, ostream);
        av_packet_unref(pkt);
    }

    return 0;
}

int encode_frames(const char* file_name, AVFrame** frame_buffer,
                  size_t frame_count) {
    const auto* codec = avcodec_find_encoder_by_name("libx264");
    // so avcodeccontext is used for both encoding and decoding...
    auto* avcc = avcodec_alloc_context3(codec);

    // assert(avcc);
    if (avcc == nullptr) {
        w_stderr("failed to allocate encoder context\n");
        return -1;
    }

    auto* pkt = av_packet_alloc();

    // Yeah this actually affects how much bitrate the
    // encode uses, it's not just metadata
    avcc->bit_rate = 400000;

    avcc->width = frame_buffer[0]->width;
    avcc->height = frame_buffer[0]->height;
    avcc->time_base = (AVRational){1, 25};
    avcc->framerate = (AVRational){25, 1};

    // avcc->gop_size = 60;
    // avcc->max_b_frames = 1;
    avcc->pix_fmt = AV_PIX_FMT_YUV420P;

    av_opt_set(avcc->priv_data, "preset", "slow", 0);

    int ret = avcodec_open2(avcc, codec, nullptr);
    if (ret < 0) {
        w_stderr("failed to open codec\n");
        return ret;
    }

    // C-style IO is needed for binary size to not explode on Windows with
    // static linking

    // TODO use unique_ptr as wrapper resource manager
    FILE* file = fopen(file_name, "wb");

    for (size_t i = 0; i < frame_count; i++) {
        // required
        frame_buffer[i]->pict_type = AV_PICTURE_TYPE_NONE;
        encode(avcc, frame_buffer[i], pkt, file);
    }
    // need to send flush packet
    encode(avcc, nullptr, pkt, file);

    (void)fclose(file);
    avcodec_free_context(&avcc);

    return 0;
}

} // namespace

constexpr size_t CHUNK_FRAME_SIZE = 60;

void encode_chunk(int chunk_idx, AVFrame** framebuf, size_t n_frames) {
    std::array<char, 64> buf{};

    // this should write null terminator
    (void)snprintf(buf.data(), buf.size(), "file %d.mp4", chunk_idx);

    encode_frames(buf.data(), framebuf, n_frames);
}

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

                auto start = now();
                int ret = run_decoder(d_ctx);
                auto elapsed_ms = since(start).count();

                if (ret == 0) {
                    auto frames = static_cast<size_t>(d_ctx.decoder->frame_num);

                    double fps = 1000.0 * (static_cast<double>(frames) /
                                           static_cast<double>(elapsed_ms));

                    printf(
                        "Successfully decoded %d frames in %lld ms (%f fps)\n",
                        (int)frames, elapsed_ms, fps);

                    // encode frames now

                    // uh... we really need a safe way to do this and stuff
                    // this is really bad... just assuming enough frames are
                    // available

                    auto start = now();

                    std::vector<std::thread> thread_vector{};
                    thread_vector.reserve((frames / CHUNK_FRAME_SIZE) + 1);

                    {
                        size_t i = 0;
                        // encode in chunks
                        for (; i < frames / CHUNK_FRAME_SIZE; i++) {

                            thread_vector.emplace_back(
                                &encode_chunk, i,
                                d_ctx.framebuf.data() + (i * CHUNK_FRAME_SIZE),
                                CHUNK_FRAME_SIZE);
                            // thread_vector[thread_vector.size() - 1].join();
                        }
                        // do remainder
                        if (frames % CHUNK_FRAME_SIZE != 0) {

                            thread_vector.emplace_back(
                                &encode_chunk, i,
                                d_ctx.framebuf.data() + (i * CHUNK_FRAME_SIZE),
                                frames % CHUNK_FRAME_SIZE);
                            // thread_vector[thread_vector.size() - 1].join();
                        }
                    }

                    for (auto& t : thread_vector) {
                        t.join();
                    }

                    auto elapsed_ms = since(start).count();
                    printf("Encoding took %lld ms\n", elapsed_ms);

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
