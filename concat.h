#pragma once

#include "decode.h"
#include "resource.h"
#include "segment.h"

#include <cstdio>
#include <libavformat/avio.h>

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

// TODO deduplicate packet copying loop.

// TODO. Deduplicate all this code between demuxers and stuff.
// Range passed is inclusive
[[nodiscard]] inline int concat_segments(unsigned int start_i,
                                         unsigned int end_i,
                                         const char* out_filename,
                                         SegmentingData seg_data) {
    DvAssert(end_i > start_i);

    int ret = 0;
    auto pkt = make_resource<AVPacket, av_packet_alloc, av_packet_free>();

    // this should work with the way smart pointers work right?
    if (pkt == nullptr) {
        return -1;
    }

    // avformat_open_input automatically frees on failure so we construct
    // the smart pointer AFTER this expression.

    // So I imagine that we do a similar thing here with creating multiple input
    // streams and just opening the concat demuxer.

    const auto* concat = av_find_input_format("concat");
    DvAssert(concat != nullptr);

    {
        std::unique_ptr<FILE, decltype(&fclose)> concat_file(
            fopen("concat.txt", "wb"), fclose);
        DvAssert(concat_file != nullptr);
        for (auto i = start_i; i <= end_i; i++) {
            DvAssert(fprintf(concat_file.get(), "file 'OUTPUT%d.mp4'\n", i) >
                     0);
        }
    }

    AVFormatContext* raw_demuxer = nullptr;
    ret = avformat_open_input(&raw_demuxer, "concat.txt", concat, nullptr);
    DvAssert(raw_demuxer != nullptr);
    if (ret < 0) {
        return ret;
    }
    auto ifmt_ctx =
        std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ctx) {
                            avformat_close_input(&ctx);
                        })>(raw_demuxer);

    avformat_find_stream_info(ifmt_ctx.get(), nullptr);

    auto video_idx = av_find_best_stream(ifmt_ctx.get(), AVMEDIA_TYPE_VIDEO, -1,
                                         -1, nullptr, 0);

    if (video_idx < 0) {
        return -1;
    }

    const AVOutputFormat* ofmt = nullptr;

    AVFormatContext* raw_ofmt_ctx = nullptr;
    avformat_alloc_output_context2(&raw_ofmt_ctx, nullptr, nullptr,
                                   out_filename);
    DvAssert(raw_ofmt_ctx != nullptr);

    auto ofmt_ctx =
        std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* ctx) {
                            avformat_free_context(ctx);
                        })>(raw_ofmt_ctx);

    ofmt = ofmt_ctx->oformat;
    DvAssert(ofmt != nullptr);

    size_t pkt_index = 0;

    {
        auto* in_stream = ifmt_ctx->streams[video_idx];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        DvAssert(in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        auto* out_stream = avformat_new_stream(ofmt_ctx.get(), nullptr);
        if (out_stream == nullptr) {
            fprintf(stderr, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            return ret;
        }
        out_stream->codecpar->codec_tag = 0;
    }

    if ((ofmt->flags & AVFMT_NOFILE) == 0) {
        // memory leak happening here...
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            // goto end;
            return ret;
            // TODO memory leak
        }
    }

    ret = avformat_write_header(ofmt_ctx.get(), nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        // goto end;
        return ret;
    }

    while (true) {
        // so usually the timestamps that av_seek_frame takes it pts,
        // but apparently some demuxers seek on dts. Not good.
        // Looks like we will have to manually seek ourselves.
        ret = av_read_frame(ifmt_ctx.get(), pkt.get());
        if (ret < 0) {
            break;
        }

        if (pkt->stream_index != video_idx) {
            // TODO determine when we actually need to unref the packet.
            av_packet_unref(pkt.get());
            continue;
        }

        // Copy timestamps from ORIGINAL video stream.
        auto ts =
            seg_data.timestamps[seg_data.packet_offsets[start_i] + pkt_index++];
        // TODO use .at() in span (C++23)
        pkt->dts = ts.dts;
        pkt->pts = ts.pts;
        pkt->pos = -1;

        ret = av_interleaved_write_frame(ofmt_ctx.get(), pkt.get());
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
    }

    av_write_trailer(ofmt_ctx.get());
    avio_close(ofmt_ctx->pb);

    return 0;
}
