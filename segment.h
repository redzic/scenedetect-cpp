#pragma once

#include <cassert>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
// #include "third_party/FFmpeg/"
}

// ok so...
// segment_end CAN be called in write_trailer,
// but with an additional flag.

extern "C" {

// how can we get this data without manually copying and pasting this...
typedef struct SegmentListEntry {
    int index;
    double start_time, end_time;
    int64_t start_pts;
    int64_t offset_pts;
    char* filename;
    struct SegmentListEntry* next;
    int64_t last_duration;
} SegmentListEntry;

typedef enum {
    LIST_TYPE_UNDEFINED = -1,
    LIST_TYPE_FLAT = 0,
    LIST_TYPE_CSV,
    LIST_TYPE_M3U8,
    LIST_TYPE_EXT, ///< deprecated
    LIST_TYPE_FFCONCAT,
    LIST_TYPE_NB,
} ListType;

// [SERIOUS] MAKE SURE THIS IS IN SYNC WITH INTERNAL FFMPEG CODE.
// THE CODE WILL NOT WORK OTHERWISE.
typedef struct SegmentContext {
    const AVClass* class_; /**< Class for private options. */
    int segment_idx; ///< index of the segment file to write, starting from 0
    int segment_idx_wrap;    ///< number after which the index wraps
    int segment_idx_wrap_nb; ///< number of time the index has wraped
    int segment_count;       ///< number of segment files already written
    const AVOutputFormat* oformat;
    AVFormatContext* avf;
    char* format; ///< format to use for output segment files
    AVDictionary* format_options;
    char* list;     ///< filename for the segment list file
    int list_flags; ///< flags affecting list generation
    int list_size;  ///< number of entries for the segment list file

    int is_nullctx;           ///< whether avf->pb is a nullctx
    int use_clocktime;        ///< flag to cut segments at regular clock time
    int64_t clocktime_offset; //< clock offset for cutting the segments at
                              // regular clock time
    int64_t clocktime_wrap_duration; //< wrapping duration considered for
                                     // starting a new segment
    int64_t last_val; ///< remember last time for wrap around detection
    int cut_pending;
    int header_written; ///< whether we've already called avformat_write_header

    char* entry_prefix;       ///< prefix to add to list entry filenames
    int list_type;            ///< set the list type
    AVIOContext* list_pb;     ///< list file put-byte context
    int64_t time;             ///< segment duration
    int64_t min_seg_duration; ///< minimum segment duration
    int use_strftime;         ///< flag to expand filename with strftime
    int increment_tc;         ///< flag to increment timecode if found

    char* times_str; ///< segment times specification string
    int64_t* times;  ///< list of segment interval specification
    int nb_times;    ///< number of elments in the times array

    char* frames_str;        ///< segment frame numbers specification string
    int* frames;             ///< list of frame number specification
    int nb_frames;           ///< number of elments in the frames array
    int frame_count;         ///< total number of reference frames
    int segment_frame_count; ///< number of reference frames in the segment

    int64_t time_delta;
    int individual_header_trailer; /**< Set by a private option. */
    int write_header_trailer;      /**< Set by a private option. */
    char* header_filename;         ///< filename to write the output header to

    int reset_timestamps; ///< reset timestamps at the beginning of each segment
    int64_t initial_offset; ///< initial timestamps offset, expressed in
                            ///< microseconds
    char* reference_stream_specifier; ///< reference stream specifier
    int reference_stream_index;
    int64_t reference_stream_first_pts; ///< initial timestamp, expressed in
                                        ///< microseconds
    int break_non_keyframes;
    int write_empty;

    int use_rename;
    char temp_list_filename[1024];

    SegmentListEntry cur_entry;
    SegmentListEntry* segment_list_entries;
    SegmentListEntry* segment_list_entries_end;
} SegmentContext;
}

// Returns 0 for success, <0 for error.
// In theory this could be parallelized a decent bit.
// First perhaps we could separate the reading of packets on the
// input stream and writing of packets on the output stream
// to be on 2 separate threads. But how necessary is that...
// Probably not really much.
[[nodiscard]] int segment_video(const char* in_filename,
                                const char* out_filename,
                                unsigned int& nb_segments) {
    const AVOutputFormat* ofmt = nullptr;
    AVFormatContext* ifmt_ctx = nullptr;
    AVFormatContext* ofmt_ctx = nullptr;
    AVPacket* pkt = nullptr;
    int ret = 0;

    SegmentContext* seg;

    // now I need to figure out how to only dump video packets...

    pkt = av_packet_alloc();
    // TODO: convert all these prints into
    if (pkt == nullptr) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        return -1;
    }

    int video_idx = -1;

    ret = avformat_open_input(&ifmt_ctx, in_filename, nullptr, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file '%s'\n", in_filename);
        // TODO: Wrap all this stuff on smart pointers so it closes
        // automatically, and return proper error enum instead of whatever this
        // is
        goto end;
    }

    ret = avformat_find_stream_info(ifmt_ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        goto end;
    }

    // av_dump_format(ifmt_ctx, 0, in_filename, 0);

    // How do I access the underlying FFOutputStream or whatever?
    avformat_alloc_output_context2(&ofmt_ctx, nullptr, "segment", out_filename);
    if (ofmt_ctx == nullptr) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    video_idx =
        av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    assert(video_idx >= 0);

    ofmt = ofmt_ctx->oformat;
    // TODO all of this should be deduplicated between segment and concat code

    {
        auto* in_stream = ifmt_ctx->streams[video_idx];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        assert(in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

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
    // av_dump_format(ofmt_ctx, 0, out_filename, 1);

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

    while (true) {
        ret = av_read_frame(ifmt_ctx, pkt);
        if (ret < 0) {
            break;
        }

        auto* in_stream = ifmt_ctx->streams[pkt->stream_index];
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }

        auto* out_stream = ofmt_ctx->streams[pkt->stream_index];

        /* copy packet */
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;

        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
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
    seg = (SegmentContext*)ofmt_ctx->priv_data;
    // printf("PRIV DATA segment: %d\n", seg->segment_count);
    printf("Final segment index: %d\n", seg->segment_count);
    nb_segments = seg->segment_count + 1;

    av_write_trailer(ofmt_ctx);

end:
    av_packet_free(&pkt);

    avformat_close_input(&ifmt_ctx);

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
