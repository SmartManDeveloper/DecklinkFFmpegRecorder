#ifndef FFMPEG_UTILS_H
#define FFMPEG_UTILS_H

extern "C" {
#include "deps/ffmpeg/include/libavutil/frame.h"
// #include "deps/ffmpeg/include/libavformat/avformat.h"
// #include "deps/ffmpeg/include/libavcodec/avcodec.h"
// #include "deps/ffmpeg/include/libavcodec/codec.h"
// #include "deps/ffmpeg/include/libavcodec/packet.h"
// #include "deps/ffmpeg/include/libavutil/avutil.h"
// #include "deps/ffmpeg/include/libavutil/samplefmt.h"
// #include "deps/ffmpeg/include/libavutil/opt.h"
// #include "deps/ffmpeg/include/libavutil/imgutils.h"
// #include "deps/ffmpeg/include/libswscale/swscale.h"
}

AVFrame *AllocateVideoFrame( AVPixelFormat pixelFormat, int width, int height  )
{
	AVFrame *frame = av_frame_alloc();
	if ( !frame )
	{
		fprintf(stderr, "Failed to allocate frame\n");
		return nullptr;
	}

	frame->format = pixelFormat;
	frame->width  = width;
	frame->height = height;

	/* allocate the buffers for the frame data */
	int ret = av_frame_get_buffer( frame, 32 );
	if ( ret < 0 )
	{
		char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
		fprintf( stderr, "Could not allocate frame data. %s\n", errorString );
		fprintf( stderr, "Params: pix_fmt %d width %d height %d\n", pixelFormat, width, height);
		av_frame_free( &frame );
		return nullptr;
	}

	return frame;
}

#endif // FFMPEG_UTILS_H