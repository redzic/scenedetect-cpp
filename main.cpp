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
// #include <errhandlingapi.h>
#include <fstream>
// #include <io.h>
#include <iostream>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>
// #include <windows.h>

#include "decode.h"

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

#define ERASE_LINE_ANSI "\x1B[1A\x1B[2K" // NOLINT

#define AlwaysInline __attribute__((always_inline)) inline

AlwaysInline void w_stderr(std::string_view sv) {
    write(STDERR_FILENO, sv.data(), sv.size());
}

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

std::atomic<uint32_t> num_frames_completed(0);                      // NOLINT
std::array<std::atomic<bool>, NUM_WORKERS> worker_threads_finished; // NOLINT

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
        // actually this doesn't really seem correct
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

AVPixelFormat av_pix_fmt_supported_version(AVPixelFormat pix_fmt) {
    switch (pix_fmt) {
    case AV_PIX_FMT_YUVJ420P:
        return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
        return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
        return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P:
        return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P:
        return AV_PIX_FMT_YUV411P;
    default:
        return pix_fmt;
    }
}

int encode_frames(const char* file_name, AVFrame* frame_buffer[],
                  size_t frame_count) {
    assert(frame_count >= 1);

    const auto* codec = avcodec_find_encoder_by_name("libaom-av1");
    // so avcodeccontext is used for both encoding and decoding...
    auto* avcc = avcodec_alloc_context3(codec);
    avcc->thread_count = THREADS_PER_WORKER;

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

    avcc->pix_fmt =
        av_pix_fmt_supported_version((AVPixelFormat)frame_buffer[0]->format);

    av_opt_set(avcc->priv_data, "cpu-used", "6", 0);
    av_opt_set(avcc->priv_data, "end-usage", "q", 0);
    av_opt_set(avcc->priv_data, "enable-qm", "1", 0);
    av_opt_set(avcc->priv_data, "cq-level", "18", 0);

    // av_opt_set(avcc->priv_data, "speed", "7", 0);

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

int encode_chunk(unsigned int chunk_idx, AVFrame* framebuf[], size_t n_frames) {
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

// clang AST transformations is something to consider
// for converting [] to .at()

void main_encode_loop(DecodeContext& d_ctx) {
    // uh... we really need a safe way to do this and stuff
    // this is really bad... just assuming enough frames are
    // available

    auto start = now();

    std::vector<std::thread> thread_vector{};
    thread_vector.reserve(NUM_WORKERS);

    // spawn worker threads
    for (unsigned int i = 0; i < NUM_WORKERS; i++) {
        thread_vector.emplace_back(&worker_thread, i, std::ref(d_ctx));
    }

    printf("frame= 0  (0 fps)\n");
    uint32_t last_frames = 0;

    auto compute_fps = [](uint32_t n_frames, int64_t time_ms) -> double {
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
            auto elapsed_local_ms = dist_ms(local_start, local_now).count();
            // TODO what happens when you cast INFINITY to int?
            // maybe fix that behavior since technically that can be
            // returned
            local_fps = (int)compute_fps(frame_diff, elapsed_local_ms);
        }

        // average fps from start of encoding process
        auto avg_fps = compute_fps(n_frames, total_elapsed_ms);

        // TODO fix int overflow, I think it happens when
        // 0ms were measured...

        // print progress
        printf(ERASE_LINE_ANSI "frame= %d  (%d fps curr, %.1f fps avg)\n",
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

    // In theory all the threads have already exited here
    // but we need to call .join() anyways.
    for (auto& t : thread_vector) {
        t.join();
    }

    // printf("\n");

    // there is no active lock on the mutex since all threads
    // terminated, so global_chunk_id can be safely accessed.
    concat_files(global_chunk_id);
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
                main_encode_loop(arg);

                // int frames = decode_loop(arg);
                // printf("decoded %d frames\n", frames);

                // assert(run_decoder(arg, 0, 60) == 60);
                // printf("initial decode of 60 frames succeeded\n");

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
