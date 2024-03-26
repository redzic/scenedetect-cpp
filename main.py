import subprocess
import cv2


def concatenate_files(max_index, output_filename, prefix):
    # Create a temporary text file to store the list of files
    with open("inputs.txt", "w") as input_file:
        for i in range(max_index + 1):
            input_file.write(f"file '{prefix}{i}.mp4'\n")

    # Build the ffmpeg command
    command = ["ffmpeg", "-f", "concat", "-safe", "0", "-i", "inputs.txt"]
    command.extend(["-c:v", "copy", "-c:a", "copy", output_filename])

    # Run the ffmpeg command with subprocess
    subprocess.run(command, check=True)

    # Delete the temporary file
    subprocess.run(["rm", "inputs.txt"], check=True)


def concatenate_file_list(file_list, output_filename):
    # Create a temporary text file to store the list of files
    with open("inputs.txt", "w") as input_file:
        for file in file_list:
            input_file.write(f"file '{file}'\n")

    # Build the ffmpeg command
    command = ["ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", "inputs.txt"]
    command.extend(["-c:v", "copy", "-c:a", "copy", output_filename])

    # Run the ffmpeg command with subprocess
    subprocess.check_output(command, stderr=subprocess.STDOUT, universal_newlines=True)

    # Delete the temporary file
    subprocess.run(["rm", "inputs.txt"], check=True)


def count_frames_mediainfo(file):
    cmd = [
        "mediainfo",
        "--Output=Video;%FrameCount%",
        file,
    ]
    output = subprocess.check_output(
        cmd, stderr=subprocess.STDOUT, universal_newlines=True
    )
    return int(output.strip())


# use ffmpeg to count frames via decoding the entire video
def count_frames_decode(file):
    # subprocess.run(["powershell", "pwd"], shell=True)

    # TODO somehow mediainfo can like legit instantly detect
    # the actual frame count...
    # Maybe just use that instead. Would make this way fucking faster.

    cmd = [
        "ffmpeg",
        "-i",
        file,
        "-f",
        "null",
        "-",
    ]
    output = subprocess.check_output(
        cmd, stderr=subprocess.STDOUT, universal_newlines=True
    )
    output = output[output.rfind("frame=") + len("frame=") :]
    output = output.lstrip()
    output = output[: output.find(" ")]
    return int(output)


def count_frames_packets(file):
    # subprocess.run(["powershell", "pwd"], shell=True)
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-count_packets",
        # "-count_frames",
        "-show_entries",
        "stream=nb_read_packets",
        # "stream=nb_read_frames",
        "-of",
        "csv=p=0",
        file,
    ]
    return int(subprocess.check_output(cmd).strip())


# (Discarded_frames_exist, num_frames)
def count_frames_packets_discard(file):
    cmd = [
        "ffprobe",
        "-i",
        file,
        "-show_packets",
    ]

    output = subprocess.check_output(
        cmd, stderr=subprocess.STDOUT, universal_newlines=True
    )
    nb_discarded = output.count("flags=_D_")

    return (nb_discarded, output.count("[PACKET]") - nb_discarded)


def main():
    sum_framecount = 0
    for i in range(29 + 1):
        seg = count_frames_packets(f"OUTPUT{i}.mp4")
        print(f"seg {i:>2}:   {seg:>4} frames")
        sum_framecount += seg

    concatenate_files(29, "concat.mkv")

    print(f"     framecount sum: {sum_framecount}")
    framecount_orig = count_frames_packets("test_x265.mkv")
    print(f"framecount original: {framecount_orig}")
    framecount_concat = count_frames_packets("concat.mkv")
    print(f"  framecount concat: {framecount_concat}")


hsh = cv2.img_hash.BlockMeanHash_create()


# if as_list=True, this function will return
# list of hashes for frames of the video
# Prints hashes for each frame.
def compute_frame_hashes(path, as_list=False):
    cap = cv2.VideoCapture(path)

    result = []
    fno = 0
    while True:
        ret, frame = cap.read()
        if not ret:
            break
        fno += 1

        frame_gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        h = hsh.compute(frame_gray)
        h2 = hash(tuple(h[0]))
        print(hex(h2))
        result.append(h2)

    if as_list:
        return result
    return hash(tuple(result))
    # return fno


def compare_videos(video_path1, video_path2):
    """
    Compares two videos frame-by-frame for exact pixel equality.

    Args:
      video_path1: Path to the first video file.
      video_path2: Path to the second video file.

    Returns:
      True if videos have the same frames and pixels, False otherwise.
    """
    cap1 = cv2.VideoCapture(video_path1)
    cap2 = cv2.VideoCapture(video_path2)

    # Check if videos opened successfully
    if not cap1.isOpened() or not cap2.isOpened():
        print("Error opening videos!")
        return False

    # Get video properties (assuming same for both)
    fps = cap1.get(cv2.CAP_PROP_FPS)
    frame_width = int(cap1.get(cv2.CAP_PROP_FRAME_WIDTH))
    frame_height = int(cap1.get(cv2.CAP_PROP_FRAME_HEIGHT))

    # Check if properties match
    if (
        # (fps != cap2.get(cv2.CAP_PROP_FPS))
        (frame_width != int(cap2.get(cv2.CAP_PROP_FRAME_WIDTH)))
        or (frame_height != int(cap2.get(cv2.CAP_PROP_FRAME_HEIGHT)))
    ):
        # print(frame_width)
        print("Video properties differ!")
        return False

    frameno = 0
    # Is this just like an opencv thing then?
    while True:
        frameno += 1
        ret1, frame1 = cap1.read()
        ret2, frame2 = cap2.read()

        if bool(ret1) != bool(ret2):
            print("Mismatched frames")
            return False

        # Check if frames read successfully
        if not ret1 or not ret2:
            break

        frame1_gray = cv2.cvtColor(frame1, cv2.COLOR_BGR2GRAY)
        frame2_gray = cv2.cvtColor(frame2, cv2.COLOR_BGR2GRAY)
        h1 = hsh.compute(frame1_gray)
        h2 = hsh.compute(frame2_gray)
        if list(h1[0]) != list(h2[0]):
            print("Hashes differ!")
            return False
        else:
            print(f"Frame {frameno}: hashes match")

        # Convert to grayscale for efficient comparison (optional)

        # Compare frames pixel-by-pixel (absolute difference)
        # diff = cv2.absdiff(frame1, frame2)

        # # Check if any pixel difference exists
        # if cv2.countNonZero(diff) > 0:
        #     print("Frames differ!")
        #     return False

    # If loop exits without finding difference, videos are identical
    print("Videos are identical!")
    return True


# TODO clean up this script so that we can automatically
# detect stuff


def f7(seq):
    seen = set()
    seen_add = seen.add
    return [x for x in seq if not (x in seen or seen_add(x))]


files = []
for i in range(1432 + 1):
    files.append(f"OUTPUT{i}.mp4")


with open("../fixes.txt") as f:
    # s = f.read()
    for line in f:
        line = line.strip()
        # print(f"orig: {line}")
        nums = line[len("OUTPUT_") :][:-4].split("_")

        # print(nums.split("_"))
        low = int(nums[0])
        high = int(nums[1])
        for i in range(low, high + 1):
            files[i] = line

files = f7(files)

# print(files)
for file in files:
    compute_frame_hashes(file)
    # print(file)


# TODO maybe find a way to copy range of timestamps from
# ORIGINAL video. Idk.

# bro these scripts are insanely messy....
# Gonna need to start making a proper debug tool.

# Example usage
# MAX_INDEX = 29
# NUM_SEG = MAX_INDEX + 1

# compute_frame_hashes("./build/out_divien.mp4")
# compute_frame_hashes("./build/out.mp4")
# compute_frame_hashes("/home/yusuf/avdist/test_x265.mp4")

# hmm so when opengop is enabled, it seems
# that the frame counts don't fully add up.
# Why is that?

# concatenate_files(MAX_INDEX, "concat_re.mp4", "bruh")

# for i in range(40):
#     subprocess.run(
#         ["ffmpeg", "-i", f"OUTPUT{i}.mp4", "-c:v", "libx264", f"bruh{i}.mp4"]
#     )
#     print(f"Reencoded segment {i}")

# yeah so somehow the frames just magically reappear
# once you concatenate them... How???

# I'll probably have to end up manually doing this GOP shit bro.


# concatenate_files(MAX_INDEX, "concat_x265_2.mp4", "OUTPUT")

# count_frames_decode("/home/yusuf/avdist/test_x265.mp4")
# count_frames_decode("/home/yusuf/avdist/OUTPUT0.mp4")

# OK.
# So there's no bug in how I decode this stuff.
# It has to do with the file being concatenated or something.
# Which is honestly really strange.

# Next step we should probably start looking at frame hashes
# to see if we can find any pattern or something.

# TODO idea for the future.
# Just try to decode those couple of packets.
# So manually move the packets to the next chunk.
# Might not work but could be worth a try.

# yeah so unfortunately fully decoding
# the stream is the ONLY way to achieve
# accurate frame count.

# I mean, we can probably
# just add a separate option for accurate segmenting
# which we can add a disclaimer that it requires
# a full decode of the stream.

# ========================== IMPORTANT SHIT.
# framesum = 0
# prev_fc = 0
# TODO keep concatenating adjacent segments until there is nothing left.
# This probably works like 99.99% of the time, but what about the edge cases, you know?

# yeah so ffmpeg basically splits on every new I-frame.
# This mostly works but sometimes causes discarded packets.
# It uses a very basic algorithm for splitting, it seems.
# So if a chunk has discarded packets, that means
# you need to merge with the NEXT chunk I believe.
# Or hmmm... UH... Idk.

# Yeah actually idk why I'm merging with the previous chunk.
# Does it just have to do with which chunk the thing gets discarded
# from? Idk.

# does ffmpeg just always cut on keyframe?
# for i in range(NUM_SEG):
#     # YES OH MY GOD. LET'S FUCKING GO.

#     nb_discard, fc = count_frames_packets_discard(f"OUTPUT{i}.mp4")
#     # x = count_frames_packets_discard(f"OUTPUT{i}.mp4")
#     # y = count_frames_packets(f"OUTPUT{i}.mp4")
#     # TODO could possibly fully reduce ffmpeg calls here
#     if nb_discard > 0 and i > 0:
#         assert i > 0
#         # means we need to connect i and i-1 as new segments
#         print(
#             f" INFO index {i}, frame counts differ: {fc+nb_discard} (all packets) - {fc} (decodable only)"
#         )
#         concatenate_file_list(
#             [f"OUTPUT{i-1}.mp4", f"OUTPUT{i}.mp4"], f"OUTPUT{i-1}_{i}.mp4"
#         )
#         print("Fixed up frames.")
#         # newchunk_frames = count_frames_packets(f"OUTPUT{i-1}_{i}.mp4")
#         newchunk_frames = prev_fc + fc + nb_discard

#         # correct values
#         # prev_fc = count_frames_packets(f"OUTPUT{i-1}.mp4")

#         framesum += newchunk_frames
#         # not entirely correct if there are two adjacent segments
#         # with wrong frame counts/packets but whatever
#         framesum -= prev_fc

#         print(
#             f"({i-1}){prev_fc} + ({i}){fc} = {prev_fc+fc}, new chunk = {newchunk_frames}"
#         )

#         prev_fc = fc
#     else:
#         assert nb_discard == 0
#         prev_fc = fc
#         framesum += fc

#     print(f"segment {i}: {fc} frames")
# print(f"final count: {framesum}")

# concatenate_file_list(["OUTPUT28.mp4", "OUTPUT29.mp4"], "output_28_29.mp4")
# print(count_frames_decode("output_28_29.mp4"))
# print(count_frames_packets("output_28_29.mp4"))

# hashes = []
# # frames_sum = 0
# for i in range(NUM_SEG):
#     #     print(f"i = {i}")
#     hashes.extend(compute_frame_hashes(f"OUTPUT{i}.mp4", as_list=True))
# frames_sum += compute_frame_hashes(f"OUTPUT{i}.mp4")
# print(frames_sum)

# ok so the actual FRAME DATA in this segmented data is the same,
# as is evident by ffmpeg concatenation.
# HOWEVER.
# for some reason opencv does not actually read the separated versions
# of the files if opengop is enabled. I do not know why.

# print(global_frameno)
# 1822214123539813135
# cap.get(cv2.CAP_PROP_FRAME_COUNT)
# hash_separate = hash(tuple(hashes))
# hash_separate = compute_frame_hashes("concat_x265.mp4")
# hash_concat = compute_frame_hashes("test_x265.mp4")
# print("Hash separate", hash_separate)
# print("Hash concat", hash_concat)
# print(hash_concat == hash_separate)

# -8794532186078781057
# -8794532186078781057
# print(compute_frame_hashes("OUTPUT0.mp4", as_list=True))
# hashes = []
# for i in range(NUM_SEG):
#     print(f"Start of segment {i}")
#     for h in compute_frame_hashes(f"OUTPUT{i}.mp4", as_list=True):
#         print(hex(h))
#     print(f"End of segment {i}")

# hashes = compute_frame_hashes("test_x265.mp4", as_list=True)

# print(hashes)
# for h in compute_frame_hashes("test_x265.mp4", as_list=True):
#     # print(f"frame {i:>2} : {h}")
#     print(hex(h))

# if compare_videos(video_path1, video_path2):
#     print("Success: Videos have the same frames and pixels.")
# else:
#     print("Videos differ in content or properties.")


# ok NOW the frame count is actually correlating... Thank god.

# bro why is the framecount of the segments less than the framecount
# of the original...
# honestly man I mean as long as they are segmented in SOME way
# and I can understand what the decoder offsets are, any segmenting
# method can work. But I do need to figure this information out tho.
