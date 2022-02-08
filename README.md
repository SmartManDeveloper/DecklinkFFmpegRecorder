# Application

Application connects to first input Blackmagic Decklink input and records first 500 video frames from it. Then it will finish video file and exit.

# Dependencies

## Blackmagic Decklink SDK

Downloaded from https://www.blackmagicdesign.com/developer

## FFmpeg (tag n5.0)

FFmpeg used in application was configured&built with commands run in shadow-build directory:
```
../ffmpeg-5/configure --enable-decklink --disable-static --enable-shared --disable-postproc --enable-avresample --enable-libx264 --disable-gpl --extra-cflags='-I$DECKLINK_INC_DIR -I/opt/ffmpeg/deps/include' --extra-ldflags='-L/opt/ffmpeg/deps/lib -lx264 -ldl' --prefix=/opt/ffmpeg/5.0/debug --enable-protocol=tls --enable-openssl --enable-libfreetype --enable-libfontconfig  --disable-stripping --disable-optimizations --extra-cflags=-Og --extra-cflags=-fno-omit-frame-pointer --enable-debug=3 --extra-cflags=-fno-inline
make -j `nproc`
make install
```



# Problem

We are recording live video using ProRes encoder. Source of video data is Blackmagicdesign Decklink Duo 2.

When we are recording with AVCodecContext thread_count set to for example 8 (on 16 core processor), movement in video (waving hand in front of camera) is causing vertical tearing of picture. Few output frames with deformed image are saved in subdirectory "_frames_".

We cannot set thread_count to 1 because of low FPS.

If I try to capture video from Blackmagic Decklink Duo 2 using ffmpeg command-line tool, it works great.
Command used:
```
ffmpeg -v verbose -f decklink -i 'DeckLink Duo (1)' -t 5 -pixel_format yuv422p10 -c:v prores -profile 1 -g 1 -an -y /tmp/prores.mov
```
