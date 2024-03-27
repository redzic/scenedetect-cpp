#pragma once

#include "resource.h"
#include <cstdint>
#include <span>
#include <vector>

// extern "C" {
// #include <libavformat/avformat.h>
// #include <libavutil/timestamp.h>
// }

// ok so...
// segment_end CAN be called in write_trailer,
// but with an additional flag.

// TODO maybe reduce size of this struct.
// How big can these values really be?
struct Timestamp {
    int64_t dts;
    int64_t pts;

    Timestamp(int64_t dts_, int64_t pts_) : dts(dts_), pts(pts_) {}
};

struct SegmentingData {
    // Wait a second. Packet
    // Packet offsets give you the base index for the timestamps.
    // Thus the length will be different.
    // The length of this vector is equal to the number of segments.
    // TODO optimization idea; get this data directly from the segment
    // muxer instead of having to manually count the packets separately.
    // This would require another ffmpeg reinterpret_cast hack though.
    std::span<uint32_t> packet_offsets;
    // timestamps[i] gives you the dts and pts of the ith packet in the
    // ORIGINAL (unsegmented) video stream.
    std::span<Timestamp> timestamps;

    SegmentingData(std::span<uint32_t> pkt_offs_, std::span<Timestamp> ts_)
        : packet_offsets(pkt_offs_), timestamps(ts_) {}
};

// inclusive range
struct ConcatRange {
    unsigned int low;
    unsigned int high;

    ConcatRange(auto low_, auto high_) : low(low_), high(high_) {
        DvAssert(low_ < high_);
    }
};

// Returns 0 for success, <0 for error.
// In theory this could be parallelized a decent bit.
// First perhaps we could separate the reading of packets on the
// input stream and writing of packets on the output stream
// to be on 2 separate threads. But how necessary is that...
// Probably not really much.
// TODO parallelize this code if possible.
[[nodiscard]] int segment_video(const char* in_filename,
                                const char* out_filename,
                                unsigned int& nb_segments,
                                std::vector<Timestamp>& timestamps);

[[nodiscard]] std::vector<ConcatRange>
fix_broken_segments(unsigned int num_segments,
                    std::vector<uint32_t>& packet_offsets,
                    std::span<Timestamp> timestamps);

// 250 frames per segment, 1024 segments
// more accurate estimate is maybe 120-250. Will depend on video of
// course.

// reasonable estimate for initial allocation amount
constexpr size_t EST_NB_SEGMENTS = 1100;

// reasonable estimate for packets per segment
constexpr size_t EST_PKTS_PER_SEG = 140;

// TODO make SURE this code works man.
// F(uint32_t segment_index)
// G(ConcatRange segment_range)
template <typename F, typename G>
inline void iter_segs(F output_i, G output_range, uint32_t nb_segments,
                      std::span<ConcatRange> segs) {
    // https://godbolt.org/z/TYj4hhjMr
    uint32_t i = 0;
    for (const auto& r : segs) {
        while (i < r.low) {
            output_i(i++);
        }
        output_range(r);
        i = r.high + 1;
    }
    while (i < nb_segments) {
        output_i(i++);
    }
}

// TODO clean up this API. It's a mess currently.
// segment video, including fixes to broken segments.
// also TODO error handling
[[nodiscard]] inline std::vector<ConcatRange>
segment_video_fully(const char* url) {
    unsigned int nb_segments = 0;
    // TODO is there ANY way to optimize this allocation?
    // Do we really need FULLY RANDOM access?
    // Can we "reset" the buffer after a concatenation has been made or
    // something?
    std::vector<Timestamp> timestamps{};
    timestamps.reserve(EST_NB_SEGMENTS * EST_PKTS_PER_SEG);
    // It would be nice to have both vectors somehow be a part of the same
    // larger allocation.
    DvAssert(segment_video(url, "OUTPUT%d.mp4", nb_segments, timestamps) == 0);

    printf("%zu - tss size (should be same as total packets)\n",
           timestamps.size());

    std::vector<uint32_t> packet_offsets{};
    packet_offsets.reserve(EST_NB_SEGMENTS);

    // TODO: remove extra debugging checks for frames,
    // or make it optional or something (eventually).

    auto segs = fix_broken_segments(nb_segments, packet_offsets, timestamps);

    return segs;
}
