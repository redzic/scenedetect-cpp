#pragma once

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

// TODO move this to its own file.

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

void fix_broken_segments(unsigned int num_segments,
                         std::vector<uint32_t>& packet_offsets,
                         std::span<Timestamp> timestamps);