#include "segment.h"
#include "concat.h"
#include "decode.h"
#include "ff_segment_muxer.h"
#include "libavformat/avformat.h"
#include "resource.h"

#include <array>
#include <cassert>

// This computes the second part of the thing
// We need the broken segments to be able to do whatever.

// TODO Also, can we dump a list of files (of concatted segments instead of
// original) and check the hashes to make sure we aren't breaking decoding
// somehow. Also for the love of god we need to clean up that python script.
// TODO we need to pass range (inclusive) instead of one parameter

// perhaps the concatenation step could be optimized by avoiding a double
// read on the input. We read and "concatenate" in the same step.
// is that possible? It might not be actually. It would probably be if
// we rolled our own concat tho. Which might not even be so hard
// if we just dump packets based on keyframes. Could maybe look into that
// in the future

void fix_broken_segments(unsigned int num_segments,
                         std::vector<uint32_t>& packet_offsets,
                         std::span<Timestamp> timestamps) {

    auto concat_files = [&packet_offsets, timestamps](unsigned int low,
                                                      unsigned int high) {
        std::array<char, 64> buf{};
        // TODO switch to mkv
        (void)snprintf(buf.data(), buf.size(), "OUTPUT_%d_%d.mp4", low, high);
        auto sd = SegmentingData(packet_offsets, timestamps);
        DvAssert(concat_segments(low, high, buf.data(), sd) == 0);

        // TODO put this in a demuxer class instead.
        auto dc = DecodeContext::open(buf.data());
        // TODO remove this call because it is just a sanity check. Or add
        // option to check that is not on by default.
        auto res = count_video_packets(std::get<DecodeContext>(dc));
        printf("  CAT[%d, %d] : %d pkts (%d decodable)\n", low, high,
               res.frame_count + res.nb_discarded, res.frame_count);

        DvAssert(res.nb_discarded == 0);
    };

    unsigned int framesum = 0;
    unsigned int nb_discarded = 0;
    uint32_t p_offset = 0;
    // index of last packet with
    unsigned int last_working = 0;
    for (unsigned int i = 0; i < num_segments; i++) {
        // Does not using the {} braces leave this totally
        // uninitialized?
        // TODO replace with {fmt}
        // and don't use iostream anywhere.
        std::array<char, 64> fpath{};
        // TODO need to find out if I'm relying on zero initialization
        // or if snprintf here outputs the null terminator.
        // TODO remove hard coded values, just operate in current folder for now
        (void)snprintf(fpath.data(), fpath.size(), "OUTPUT%d.mp4", i);

        // TODO use DemuxerContext once that works properly
        // So that we don't have to waste time initializing a decoder when
        // we don't need one.
        auto vdec = DecodeContext::open(fpath.data());
        // TODO make sure with all this stuff everything correctly gets
        // closed and stuff
        // TODO error handling: access variant properly.
        // TODO Can the broken packets be identified while segmenting? with some
        // low overhead method?
        auto frames = count_video_packets(std::get<DecodeContext>(vdec));
        printf("[%d]frames: %d\n", i, frames.frame_count);

        packet_offsets.push_back(p_offset);
        p_offset += frames.frame_count + frames.nb_discarded;

        if (i == 0) [[unlikely]] {
            DvAssert(frames.nb_discarded == 0);
        }
        // Ideally we should manually split up the loop with lambdas and such.
        // But realistically it doesn't actually matter.

        // Concats are using inclusive range.

        if (frames.nb_discarded == 0) [[likely]] {
            if (i != 0 && last_working != (i - 1)) [[unlikely]] {
                printf("  CONCAT NEEDED: [%d, %d]\n", last_working, i - 1);
                concat_files(last_working, i - 1);
            }
            last_working = i;
        }

        // check last iteration, otherwise it doesn't get handled
        if (i == (num_segments - 1)) [[unlikely]] {
            if (last_working != i) {
                printf("  CONCAT NEEDED: [%d, %d]\n", last_working, i);
                concat_files(last_working, i);
            }
        }

        if (frames.nb_discarded != 0) {
            printf(" [%d INFO]  frame counts differ: %d (all packets) - %d "
                   "(decodable only)\n",
                   i, frames.frame_count + frames.nb_discarded,
                   frames.frame_count);
            nb_discarded += frames.nb_discarded;
        }

        // frame count + discarded gives total packet count (guaranteed)

        DvAssert(frames.frame_count > 0);
        framesum += frames.frame_count;
    }

    // Clang split loop?

    // loop end reached

    printf("Framesum: %d\nTotal packets: %d\n", framesum,
           framesum + nb_discarded);
}

// I think we need to get the segmenting data out of this function.
int segment_video(const char* in_filename, const char* out_filename,
                  unsigned int& nb_segments,
                  std::vector<Timestamp>& timestamps) {
    // TODO move this to DemuxerContext
    const AVOutputFormat* ofmt = nullptr;
    AVFormatContext* ofmt_ctx = nullptr;

    AVStream* in_stream = nullptr;
    AVStream* out_stream = nullptr;

    SegmentContext* seg = nullptr;

    auto pkt = make_resource<AVPacket, av_packet_alloc, av_packet_free>();

    int video_idx = -1;

    // TODO deduplicate this code
    AVFormatContext* raw_demuxer_input = nullptr;

    int ret =
        avformat_open_input(&raw_demuxer_input, in_filename, nullptr, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file '%s'\n", in_filename);
        // TODO: Wrap all this stuff on smart pointers so it closes
        // automatically, and return proper error enum instead of whatever this
        // is
        return ret;
    }

    auto ifmt_ctx =
        std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ptr) {
                            avformat_close_input(&ptr);
                        })>(raw_demuxer_input);

    ret = avformat_find_stream_info(ifmt_ctx.get(), nullptr);
    if (ret < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        goto end;
    }

    // How do I access the underlying FFOutputStream or whatever?
    avformat_alloc_output_context2(&ofmt_ctx, nullptr, "segment", out_filename);
    if (ofmt_ctx == nullptr) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    video_idx = av_find_best_stream(ifmt_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                    nullptr, 0);
    DvAssert(video_idx >= 0);

    ofmt = ofmt_ctx->oformat;
    // TODO all of this should be deduplicated between segment and concat code

    {
        auto* in_stream = ifmt_ctx->streams[video_idx];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        DvAssert(in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        auto* out_stream = avformat_new_stream(ofmt_ctx, nullptr);
        if (out_stream == nullptr) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            goto end;
        }
        out_stream->codecpar->codec_tag = 0;
    }

    if ((ofmt->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            goto end;
        }
    }

    // yeah so we REALLY can't rely on filenames to correctly
    // detect the number of segments written, unfortunately.

    ret = avformat_write_header(ofmt_ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
    }

    in_stream = ifmt_ctx->streams[video_idx];
    out_stream = ofmt_ctx->streams[video_idx];

    while (true) {
        // TODO move out of loop.
        // Originally this was inside the loop. I beileve this is fine?
        ret = av_read_frame(ifmt_ctx.get(), pkt.get());
        if (ret < 0) {
            break;
        }

        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt.get());
            continue;
        }

        timestamps.emplace_back(pkt->dts, pkt->pts);

        /* copy packet */
        av_packet_rescale_ts(pkt.get(), in_stream->time_base,
                             out_stream->time_base);
        pkt->pos = -1;

        ret = av_interleaved_write_frame(ofmt_ctx, pkt.get());
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
        av_packet_unref(pkt.get());
    }

    // so I believe it's not possible for this here to be
    // incremented more than 1 after the trailer is written.
    // Is that correct??
    seg = reinterpret_cast<SegmentContext*>(ofmt_ctx->priv_data);
    printf("Final segment index: %d\n", seg->segment_count);
    nb_segments = seg->segment_count + 1;

    av_write_trailer(ofmt_ctx);

end:

    /* close output */
    if (ofmt_ctx != nullptr && ((ofmt->flags & AVFMT_NOFILE) == 0)) {
        avio_closep(&ofmt_ctx->pb);
    }
    avformat_free_context(ofmt_ctx);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return ret;
    }

    return 0;
}