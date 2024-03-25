#pragma once

#include "decode.h"
#include "resource.h"
#include "segment.h"

#include <cassert>
#include <cstdio>
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

// alright I'm gonna manually check
// what can be done about these timestamps.

// Something needs to be done about these damn timestamps man.

// So I think after segmenting the ORIGINAL timestamps need to be
// copied from the video.

// or perhaps these should be string_views
// concats i-1 and i
// TODO take span as argument not vector reference

// I believe all this code originally came from
// the remux example in ffmpeg.
// TODO also deduplicate packet copying loop.

// [[nodiscard]] inline int concat_video(unsigned int i, const char*
// out_filename,
// range is inclusive
[[nodiscard]] inline int concat_video(unsigned int start_i, unsigned int end_i,
                                      const char* out_filename,
                                      SegmentingData seg_data) {

    assert(end_i > start_i);

    int ret = 0;
    auto pkt = make_resource<AVPacket, av_packet_alloc, av_packet_free>();

    // this should work with the way smart pointers work right?
    if (pkt == nullptr) {
        return -1;
    }

    // TODO. Deduplicate all this code between demuxers and stuff.

    AVFormatContext* raw_demuxer = nullptr;

    // avformat_open_input automatically frees on failure so we construct
    // the smart pointer AFTER this expression.

    // So I imagine that we do a similar thing here with creating multiple input
    // streams and just opening the concat demuxer.

    const auto* concat = av_find_input_format("concat");
    assert(concat != nullptr);

    {
        std::unique_ptr<FILE, decltype(&fclose)> concat_file(
            fopen("concat.txt", "wb"), fclose);
        assert(concat_file != nullptr);
        for (auto i = start_i; i <= end_i; i++) {
            assert(fprintf(concat_file.get(), "file 'OUTPUT%d.mp4'\n", i) > 0);
        }
    }

    {

        // the open input needs to be of the concat file bruh
        ret = avformat_open_input(&raw_demuxer, "concat.txt", concat, nullptr);
        if (ret < 0) {
            return ret;
        }
    }

    assert(raw_demuxer != nullptr);
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

    AVFormatContext* ofmt_ctx = nullptr;
    avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, out_filename);
    if (ofmt_ctx == nullptr) {
        fprintf(stderr, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    ofmt = ofmt_ctx->oformat;
    assert(ofmt != nullptr);

    size_t pkt_index = 0;

    {
        auto* in_stream = ifmt_ctx->streams[video_idx];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        assert(in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

        auto* out_stream = avformat_new_stream(ofmt_ctx, nullptr);
        if (out_stream == nullptr) {
            fprintf(stderr, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            // goto end;
            // TODO fix memory leak
            return ret;
        }
        out_stream->codecpar->codec_tag = 0;
    }

    if ((ofmt->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            // goto end;
            return ret;
            // TODO memory leak
        }
    }

    ret = avformat_write_header(ofmt_ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        // goto end;
        return ret;
    }

    while (true) {
        // TODO rename demuxer
        // so usually the timestamps that av_seek_frame takes it pts,
        // but apparently some demuxers seek on dts. Not good.
        // Looks like we will have to manually seek ourselves.
        ret = av_read_frame(ifmt_ctx.get(), pkt.get());
        if (ret < 0) {
            break;
        }

        // auto* in_stream = ifmt_ctx->streams[pkt->stream_index];
        if (pkt->stream_index != video_idx) {
            // TODO determine when we actually need to unref the packet.
            av_packet_unref(pkt.get());
            continue;
        }

        // auto* out_stream = ofmt_ctx->streams[pkt->stream_index];

        // TODO should probably add bounds check here
        // Copy timestamps from ORIGINAL
        auto ts =
            seg_data.timestamps[seg_data.packet_offsets[start_i] + pkt_index++];
        // .timestamps[seg_data.packet_offsets[start_i - 1] + pkt_index++];
        // TODO use .at() in spam (C++23)
        // auto ts = seg_data.timestamps.at(seg_data.packet_offsets.at(i - 1) +
        //                                  pkt_index++);
        pkt->dts = ts.dts;
        pkt->pts = ts.pts;
        pkt->pos = -1;

        ret = av_interleaved_write_frame(ofmt_ctx, pkt.get());
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
    }

    // so I believe it's not possible for this here to be
    // incremented more than 1 after the trailer is written.
    // Is that correct??

    av_write_trailer(ofmt_ctx);

    // eh whatever we'll write the code to do all this later

    return 0;
}
