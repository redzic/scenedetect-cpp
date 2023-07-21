#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <io.h>
#include <iostream>
#include <memory>
#include <mutex>
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
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace {

#define AlwaysInline __attribute__((always_inline)) inline

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

constexpr size_t CHUNK_FRAME_SIZE = 60;
constexpr size_t NUM_WORKERS = 16;

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

// how do you make a static allocation?

// Move cursor up and erase line
#define ERASE_LINE_ANSI "\x1B[1A\x1B[2K" // NOLINT

// bounds checking is not performed here
int run_decoder(DecodeContext& dc, size_t framebuf_offset, size_t max_frames) {
    if ((framebuf_offset + max_frames - 1) > framebuf_size) {
        return -1;
    }

    // AVCodecContext allocated with alloc context
    // previously was allocated with non-NULL codec,
    // so we can pass NULL here.
    int ret = avcodec_open2(dc.decoder, nullptr, nullptr);
    if (ret < 0) [[unlikely]] {
        return ret;
    }

    size_t output_index = 0;

    // returns 0 on success, or negative averror
    auto receive_frames = [&dc, max_frames, framebuf_offset,
                           &output_index]() mutable {
        // receive last frames
        while (output_index < max_frames) {
            int ret = avcodec_receive_frame(
                dc.decoder, dc.framebuf[output_index + framebuf_offset]);
            if (ret < 0) [[unlikely]] {
                return ret;
            }

            output_index++;
        }
        return 0;
    };

    while (true) {
        // Flush any frames currently in the decoder
        //
        // This is needed in case we send a packet, read some of its frames and
        // stop because of max_frames, and need to keep reading frames from the
        // decoder on the next chunk.

        // TODO deduplicate this code
        ret = receive_frames();
        if (ret == AVERROR_EOF) [[unlikely]] {
            return (int)output_index;
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) [[unlikely]] {
            return ret;
        } else [[likely]] {
        }

        // Get packet (compressed data) from demuxer
        ret = av_read_frame(dc.demuxer, dc.pkt);
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

            printf("Error decoding frame!\n");

            return ret;
        } else {
            av_packet_unref(dc.pkt);
        }

        // receive as many frames as possible up until max size
        // TODO check error?
        ret = receive_frames();
        if (ret == AVERROR_EOF) [[unlikely]] {
            return (int)output_index;
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) [[unlikely]] {
            return ret;
        } else [[likely]] {
        }

        if (output_index >= max_frames) [[unlikely]] {
            return (int)output_index;
        }
    }

    // once control flow reached here, it is guaranteed that more frames need to
    // be received to fill the buffer send flush packet
    // TODO error handling here as well
    avcodec_send_packet(dc.decoder, nullptr);

    ret = receive_frames();
    if (ret == AVERROR_EOF || ret == 0) {
        return (int)output_index;
    } else {
        return ret;
    }
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

template <class result_t = std::chrono::milliseconds,
          class clock_t = std::chrono::steady_clock,
          class duration_t = std::chrono::milliseconds>
auto dist_ms(std::chrono::time_point<clock_t, duration_t> const& start,
             std::chrono::time_point<clock_t, duration_t> const& end) {
    return std::chrono::duration_cast<result_t>(end - start);
}

auto now() { return std::chrono::steady_clock::now(); }

std::mutex global_decoder_mutex;  // NOLINT
unsigned int global_chunk_id = 0; // NOLINT

// for printing progress
std::condition_variable cv; // NOLINT
std::mutex cv_m;            // NOLINT

std::atomic<uint32_t> num_frames_completed(0); // NOLINT
std::array<std::atomic<bool>, NUM_WORKERS> worker_threads_finished;

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

        // just printing this takes 100ms...
        // printf("received packet from encoder of %d bytes\n", pkt->size);

        av_packet_unref(pkt);
    }

    return 0;
}

int encode_frames(const char* file_name, AVFrame** frame_buffer,
                  size_t frame_count) {
    const auto* codec = avcodec_find_encoder_by_name("librav1e");
    // so avcodeccontext is used for both encoding and decoding...
    auto* avcc = avcodec_alloc_context3(codec);
    avcc->thread_count = 4;

    // assert(avcc);
    if (avcc == nullptr) {
        w_stderr("failed to allocate encoder context\n");
        return -1;
    }

    auto* pkt = av_packet_alloc();

    // this affects how much bitrate the
    // encoder uses, it's not just metadata
    // avcc->bit_rate = static_cast<int64_t>(40000) * 3;

    avcc->width = frame_buffer[0]->width;
    avcc->height = frame_buffer[0]->height;
    avcc->time_base = (AVRational){1, 25};
    avcc->framerate = (AVRational){25, 1};

    // avcc->max_b_frames = 1;
    avcc->pix_fmt = AV_PIX_FMT_YUV420P;

    // av_opt_set(avcc->priv_data, "preset", "slow", 0);

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
        num_frames_completed++;
    }
    // need to send flush packet
    encode(avcc, nullptr, pkt, file);

    (void)fclose(file);
    avcodec_free_context(&avcc);

    return 0;
}

int encode_chunk(unsigned int chunk_idx, AVFrame** framebuf, size_t n_frames) {
    std::array<char, 64> buf{};

    // this should write null terminator
    (void)snprintf(buf.data(), buf.size(), "file %d.mp4", chunk_idx);

    return encode_frames(buf.data(), framebuf, n_frames);
}

// framebuf is start of frame buffer that worker can use
int worker_thread(unsigned int worker_id, DecodeContext& decoder) {
    while (true) {
        // should only access decoder once lock has been acquired
        global_decoder_mutex.lock();

        // decode CHUNK_FRAME_SIZE frames into frame buffer
        int frames = run_decoder(decoder, worker_id * CHUNK_FRAME_SIZE,
                                 CHUNK_FRAME_SIZE);

        // error decoding
        if (frames <= 0) {
            worker_threads_finished[worker_id].store(true);
            cv.notify_one();
            global_decoder_mutex.unlock();
            return frames;
        }

        // these accesses are behind mutex so we're all good
        auto chunk_idx = global_chunk_id;
        // increment for next chunk
        global_chunk_id++;

        // can assume frames are available, so unlock the mutex so
        // other threads can use the decoder
        global_decoder_mutex.unlock();

        // a little sketchy but in theory this should be fine
        // since framebuf is never modified
        int ret = encode_chunk(
            chunk_idx, decoder.framebuf.data() + (worker_id * CHUNK_FRAME_SIZE),
            frames);

        if (ret != 0) {
            // in theory... this shouldn't need to happen as this is an encoding
            // error

            worker_threads_finished[worker_id].store(true);

            // in normal circumstances we return from infinite loop via decoding
            // error (which we expect to be EOF).
            cv.notify_one();

            return ret;
        }
    }
}

void avlog_do_nothing(void* /* unused */, int /* level */,
                      const char* /* szFmt */, va_list /* varg */) {}

// assume same naming convention
void concat_files(unsigned int num_files) {
    std::array<char, 64> buf{};

    std::ofstream dst("output.mp4", std::ios::binary);

    // TODO convert to faster loop using read/write directly
    for (unsigned int i = 0; i < num_files; i++) {
        (void)snprintf(buf.data(), buf.size(), "file %d.mp4", i);
        std::ifstream src(buf.data(), std::ios::binary);
        dst << src.rdbuf();
        src.close();
    }

    dst.close();
}

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

    av_log_set_callback(avlog_do_nothing);

    std::visit(
        [](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, DecodeContext>) {
                auto& d_ctx = arg;

                // encode frames now

                // uh... we really need a safe way to do this and stuff
                // this is really bad... just assuming enough frames are
                // available

                auto start = now();

                std::vector<std::thread> thread_vector{};
                thread_vector.reserve(NUM_WORKERS);

                // spawn worker threads
                for (unsigned int i = 0; i < NUM_WORKERS; i++) {
                    thread_vector.emplace_back(&worker_thread, i,
                                               std::ref(d_ctx));
                }

                printf("frame= 0  (0 fps)\n");
                uint32_t last_frames = 0;

                auto compute_fps = [](uint32_t n_frames,
                                      int64_t time_ms) -> double {
                    if (time_ms <= 0) [[unlikely]] {
                        return INFINITY;
                    } else [[likely]] {
                        return static_cast<double>(1000 * n_frames) /
                               static_cast<double>(time_ms);
                    }
                };

                while (true) {
                    // acquire lock on mutex I guess?
                    std::unique_lock<std::mutex> lk(cv_m);

                    // so for some reason this notify system isn't good enough
                    // for checking if all threads have completed.
                    auto local_start = now();
                    auto status = cv.wait_for(lk, std::chrono::seconds(1));

                    auto n_frames = num_frames_completed.load();
                    auto frame_diff = n_frames - last_frames;
                    last_frames = n_frames;
                    // since we waited exactly 1 second, frame_diff is the fps.

                    // actually, it's not guaranteed that we waited for exactly
                    // 1 second... so let's measure it

                    auto local_now = now();
                    auto total_elapsed_ms = dist_ms(start, local_now).count();

                    uint32_t local_fps = frame_diff;
                    if (status == std::cv_status::no_timeout) [[unlikely]] {
                        // this means we didn't wait for the full time
                        auto elapsed_local_ms =
                            dist_ms(local_start, local_now).count();
                        // TODO what happens when you cast INFINITY to int?
                        // maybe fix that behavior since technically that can be
                        // returned
                        local_fps =
                            (int)compute_fps(frame_diff, elapsed_local_ms);
                    }

                    // average fps from start of encoding process
                    auto avg_fps = compute_fps(n_frames, total_elapsed_ms);

                    // print progress
                    printf(ERASE_LINE_ANSI
                           "frame= %d  (%d fps curr, %.1f fps avg)\n",
                           n_frames, local_fps, avg_fps);

                    static_assert(NUM_WORKERS >= 1);
                    bool are_threads_finished = true;
                    for (auto& is_tf : worker_threads_finished) {
                        if (!is_tf.load()) {
                            are_threads_finished = false;
                            break;
                        }
                    }

                    if (are_threads_finished) {
                        break;
                    }
                }

                for (auto& t : thread_vector) {
                    t.join();
                }

                // there is no active lock on the mutex since all threads
                // terminated, so global_chunk_id can be safely accessed.
                concat_files(global_chunk_id);

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
