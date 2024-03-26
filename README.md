# Chunked Encoder

Only supports the latest build of FFmpeg libaries.
This is due to hacks/workarounds used when dealing with
libav. It would be a big hassle to support multiple versions.

- Do not use C++ streams (iostream,fstream). They are very slow.
- use cppfront

TODO:
- add experimental disclaimer with possibly not working codecs
  and all that
    - Add basic tests
    - Add fuzzing
    - Test with ASan/UBSan/all that stuff
    - static Analysis. Cppcheck, pvs studio
    - Compile with multiple compilers

    - Randomly generated inputs
        - Using many different encoders.
        - Lossless. Check PSNR.
        - This will test the encode/decode loop logic.

    - Find suite of inputs I can test on. Maybe look into ffmpeg's
      test suite.

    - Automatically detect uncompressed input.
        - Or at least very large videos and display a warning or something.

    - Look into optimizing how allocations are done in various parts of the code.

    - Maybe look for an allocator that can detect leaks and double frees and all that
      kind of stuff.

  - Clang AST transformation to, for example, make sure that if you call a function
    like avcodec_receive_frame(), you have to check its return value?

  - Test functions like concatenations that they ACTUALLY produce valid bitstreams.
    Find some stuff for it and do like randomized decoder loop checking. Not just
    check one single time in mpv and it "seems to have worked".

  - make sure windows builds are compiled with clang.