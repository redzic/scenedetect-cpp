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
#include <mutex>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

#include "concat.h"
#include "decode.h"
#include "resource.h"
#include "segment.h"

extern "C" {

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavfilter/avfilter.h>
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

AlwaysInline void w_err(std::string_view sv) {
    write(STDERR_FILENO, sv.data(), sv.size());
}

// so it seems like you have to call
// unref before you reuse AVPacket or AVFrame

void segvHandler(int /*unused*/) {
    w_err("Segmentation fault occurred. Please file a bug report on GitHub.\n");
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
// is mutex fast?
// is memcpy optimized on windows? Memchr sure isn't.
// is there a "runtime" that provides optimized implementations?
// can I override the standard memcpy/memchr functions?
std::mutex cv_m; // NOLINT

// are exceptions slow? What's the fastest way to do
// error handling?

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
    DvAssert(frame_count >= 1);

    const auto* codec = avcodec_find_encoder_by_name("libaom-av1");
    // so avcodeccontext is used for both encoding and decoding...
    auto* avcc = avcodec_alloc_context3(codec);
    avcc->thread_count = THREADS_PER_WORKER;

    // DvAssert(avcc);
    if (avcc == nullptr) {
        w_err("failed to allocate encoder context\n");
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
        w_err("failed to open codec\n");
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

// void avlog_do_nothing(void* /* unused */, int /* level */,
//                       const char* /* szFmt */, va_list /* varg */) {}

// so I mean as hack we could do this dumb shit
// with checking the callback stuff...
// void av_log_TEST_CALLBACK(void* ptr, int level, const char* fmt, va_list vl)
// {
//     printf("%s\n", fmt);
// }

// assume same naming convention
// this is direct concatenation, nothing extra done to files.
// hardcoded. TODO remove.
void raw_concat_files(unsigned int num_files) {
    std::array<char, 64> buf{};

    std::ofstream dst("output.mp4", std::ios::binary);

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
    raw_concat_files(global_chunk_id);
}

// each index tells you the packet offset for that segment
// for both dts and pts, of course.

// this code is so incredibly messy bro

// I guess maybe this should iterate backwards
// from max index to 0, that way when a thing happens.
// we can avoid double counting the segments. oR wait...
// maybe not.

// basically what we need need to do
// is build these vectors
// TODO move this into segment.h

// I think we just need to configure the AVOutputFormat propertly.
// It seems like if this is null, the output format is guessed
// based on the file extension.

// TODO: replace manual loop over streams with av_find_best_stream or whatever

// For concat I really don't know if I should use the concat
// filter or just directly put the packets next to each other.
// Probably better to use concat filter I guess.

// reasonable estimate for initial allocation amount
constexpr size_t EST_NB_SEGMENTS = 1100;

// reasonable estimate for packets per segment
constexpr size_t EST_PKTS_PER_SEG = 140;

// BE CAREFUL NOT TO WRITE ASSERTS
// WITH SIDE EFFECTS.

// so now that we got the segmenting code working I guess it's time to
// write up the TCP server and client.

// TODO We should add a test for this, and measure "decoder stalling".
// There's also a problem of what if we need really long segments plus short
// segments. Then our current model of using one global decoder isn't so good,
// also because we would have to allocate the entire chunk ahead of time.
// which would be a huge memory problem obviously. It works ok with fixed sized
// chunks though.

// I think there's a solution to fix the memory hogging problem,
// but it would still require having the decoder available.
//      (the fix is to decode n frames at a time and cycling
//      between encoder and decoder instead of full decode + Encode step.)

// so unique_ptr DOES seem to be "zero cost" if you use make_unique.
// in certain cases at least. and noexcept.

// TODO: if I REALLY wanna make this code complex,
// I could implement some kind of way to decode packets
// as they are coming in instead of waiting for the entire chunk to
// be received first.

// also I should check for if

// https://github.com/facebook/wdt
// perhaps we should consider this.

// Perhaps we should just use one global reader, to minimize latency.
// But hmm, perhaps that's not the best approach either, because one client
// may not have the best data connection speed.
// The best approach would be rather complex.
// We should at least be maxing out our bandwidth.

// we could also possibly look into using bittorrent.

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/write.hpp>
#include <cstdio>
#include <iostream>

using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;
using asio::ip::tcp;
namespace this_coro = asio::this_coro;

#if defined(ASIO_ENABLE_HANDLER_TRACKING)
#define use_awaitable                                                          \
    asio::use_awaitable_t(__FILE__, __LINE__, __PRETTY_FUNCTION__)
#endif

awaitable<void> echo(tcp::socket socket) {
    try {
        char data[1024];
        for (;;) {
            std::size_t n = co_await socket.async_read_some(asio::buffer(data),
                                                            use_awaitable);
            co_await async_write(socket, asio::buffer(data, n), use_awaitable);
        }
    } catch (std::exception& e) {
        // I guess this is ok since it should be "TRULY" exceptional.
        printf("echo Exception: %s\n", e.what());
    }
}

awaitable<void> listener() {
    auto executor = co_await this_coro::executor;
    tcp::acceptor acceptor(executor, {tcp::v4(), 55555});
    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
        co_spawn(executor, echo(std::move(socket)), detached);
    }
}

// TODO.
// perhaps we could avoid the overhead of redundantly reading from files.
// By just storing the compressed data in memory.

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Must specify 2 args.\n");
        return -1;
    }

    // ok now we have our actual TCP server/client setup here.
    auto mode = std::string_view(argv[1]);
    try {

        if (mode == "server") {
            asio::io_context io_context;

            tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 7878));

            printf("Listening for connections (synchronous)...\n");
            for (;;) {
                tcp::socket socket(io_context);
                acceptor.accept(socket);

                printf("[TCP] Connection accepted\n");

                std::string message = "hello there!";
                std::array<char, 64> recv_message{};

                asio::error_code ignored_error;
                // So this writes ALL of the data before returning, apparently.
                asio::write(socket, asio::buffer(message, 1), ignored_error);
                // I believe here we are closing the connection (through
                // dtor)
                auto nr =
                    socket.read_some(asio::buffer(recv_message), ignored_error);
                // yeah so read reads ALL the bytes. read_some only reads some.
                // Maybe read up on how this read stuff is determined.
                std::cout << "Read data from client: "
                          << std::string_view(recv_message.data(), nr) << '\n';
            }

        } else if (mode == "client") {
            asio::io_context io_context;
            tcp::resolver resolver(io_context);

            auto endpoints = resolver.resolve("localhost", "7878");

            tcp::socket socket(io_context);
            asio::connect(socket, endpoints);

            std::string_view data_to_send = "Howdy! I'm jack!";
            for (;;) {
                std::array<char, 128> buf{};
                asio::error_code error;

                size_t len = socket.read_some(asio::buffer(buf), error);

                if (error == asio::error::eof) {
                    break; // Connection closed cleanly by peer (server).
                } else if (error) {
                    throw asio::system_error(error); // Some other error.}
                }
                // data ends up in the buffer here.
                std::cout.write(buf.data(), len);

                // how do I write data back?
                asio::write(socket, asio::buffer(data_to_send), error);
            }
        } else {
            printf("unknown mode %.*s.\n", (int)mode.size(), mode.data());
        }

    } catch (std::exception& e) {
        printf("Exception: %s\n", e.what());
    }
}

int main_unused(int argc, char* argv[]) {
    if (signal(SIGSEGV, segvHandler) == SIG_ERR) {
        w_err("signal(): failed to set SIGSEGV signal handler\n");
    }

    if (argc != 2) {
        w_err("DiViEn: invalid number of arguments\n"
              "   usage: DiViEn  <video_file>\n");
        return -1;
    }
    const char* url = argv[1];

    // I think maybe next step will be cleaning up the code. So that it will
    // be feasible to continue working on this.

    // TODO maybe add option to not copy timestamps.
    // Dang this stuff is complicated.

    // 1432 max index

    // ok so unfortunately it does seem like it's possible to
    // have totally adjacent segments with broken framecounts.
    // In which case I believe we need to concatenate longer
    // segments.
    // But hopefully it isn't possible that the very first segment
    // has extra packets.

    unsigned int nb_segments = 0;
    std::vector<Timestamp> timestamps{};
    // 250 frames per segment, 1024 segments
    // more accurate estimate is maybe 120-250. Will depend on video of
    // course.
    timestamps.reserve(EST_NB_SEGMENTS * EST_PKTS_PER_SEG);
    // It would be nice to have both vectors somehow be a part of the same
    // larger allocation.
    // BRUH...
    DvAssert(segment_video(url, "OUTPUT%d.mp4", nb_segments, timestamps) == 0);

    printf("%zu - seg size\n", timestamps.size());

    std::vector<uint32_t> packet_offsets{};
    packet_offsets.reserve(EST_NB_SEGMENTS);
    // LETS GO DUDE. FINALLY IT'S WORKING.

    // TODO: remove extra debugging checks for frames,
    // or make it optional or something (eventually).

    fix_broken_segments(nb_segments, packet_offsets, timestamps);

    return 0;

    // av_log_set_level(AV_LOG_VERBOSE);

    // segment_video("/home/yusuf/avdist/test_x265.mp4", "OUTPUT%d.mp4");
    // so concat code works perfectly fine
    // unsigned int nb_segments = 30;

    // TODO: Dynamically count number of segments made.
    // Is there a way to do that from ffmpeg directly?
    // identify_broken_segments(nb_segments);

    // can assume argv[1] is now available

    // so as soon as you use something like std::cout, the binary size
    // increases greatly...

    // so we should probably find a way to not use things that increase the
    // binary size a lot...

    // bro WHY are there dropped frames...
    // when decoding man... I just don't get all of them.
    return 0;

    // TODO allow setting log level via CLI
    // av_log_set_callback(avlog_do_nothing);
    // should I just modify the ffmpeg library to do what
    // I need it to do? Hmm... Well it won't work with the system
    // version of the library then, unfortunately.
    av_log_set_callback(av_log_default_callback);
    // av_log_default_callback();

    // TODO move all this to another function

    auto vdec = DecodeContext::open(url);

    // TODO use std::expected instead?
    std::visit(
        [](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, DecodeContext>) {
                main_encode_loop(arg);

                // int frames = decode_loop(arg);
                // printf("decoded %d frames\n", frames);

                // DvAssert(run_decoder(arg, 0, 60) == 60);
                // printf("initial decode of 60 frames succeeded\n");

            } else if constexpr (std::is_same_v<T, DecoderCreationError>) {
                auto error = arg;

                // TODO move all this into its own function
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
