#include "recorder.h"

#include <stdio.h>
#include <QQueue>
#include <QThreadPool>
#include <QFuture>
#include <QtConcurrent/QtConcurrent>

extern "C" {
#include "deps/ffmpeg/include/libavformat/avformat.h"
#include "deps/ffmpeg/include/libavcodec/avcodec.h"
#include "deps/ffmpeg/include/libavcodec/codec.h"
#include "deps/ffmpeg/include/libavcodec/packet.h"
#include "deps/ffmpeg/include/libavutil/avutil.h"
#include "deps/ffmpeg/include/libavutil/frame.h"
#include "deps/ffmpeg/include/libavutil/samplefmt.h"
#include "deps/ffmpeg/include/libavutil/opt.h"
#include "deps/ffmpeg/include/libavutil/imgutils.h"
#include "deps/ffmpeg/include/libswscale/swscale.h"
}

#include "ffmpegutils.h"

///@cond INTERNAL

static const char *VIDEO_OUTPUT_FILE = "/tmp/testing.mov";

#define __RECORD_WITH_PRORES__ 1
#define __RECORD_WITH_X264__ 0
#define __BMD_TO_AVFRAME__ 0
#define __BMD_TO_PACKET__ 1

class Recorder::PrivateClass
{
public:
	uint64_t mFrameCount = 0;

	uint16_t mVideoWidth = 1920;
	uint16_t mVideoHeight = 1080;
	AVCodecID mInputVideoCodec = AV_CODEC_ID_RAWVIDEO;
	AVPixelFormat mInputPixelFormat = AV_PIX_FMT_UYVY422;
#if __RECORD_WITH_PRORES__
	AVPixelFormat mPixelFormat = AV_PIX_FMT_YUV422P10LE;
	AVCodecID mVideoCodec = AV_CODEC_ID_PRORES;
#elif __RECORD_WITH_X264__
	AVPixelFormat mPixelFormat = AV_PIX_FMT_YUV420P;
	AVCodecID mVideoCodec = AV_CODEC_ID_H264;
#endif
	AVCodecID mAudioCodec = AV_CODEC_ID_PCM_S16LE;
	AVRational mTimeBase = {1, 1};

	const AVOutputFormat *mOutputFormat = nullptr;
	AVFormatContext *mFormatContext = nullptr;
	AVStream *mAudioStream = nullptr;
	AVStream *mVideoStream = nullptr;
	AVCodecContext *mVideoDecodingContext = nullptr;
	AVCodecContext *mAudioCodecContext = nullptr;
	AVCodecContext *mVideoCodecContext = nullptr;
	AVFrame *mVideoEncodingFrame = nullptr;
	SwsContext *mSwScaleContext = nullptr;

	std::atomic_bool mCaptureActive;
	QThreadPool mThreadPool;
	QFuture<void> mDecodingThread;
	QFuture<void> mEncodingThread;
	QFuture<void> mFileWritingThread;

	QMutex mDecodePacketQueueMutex;
	QQueue<AVPacket *> mDecodePacketQueue;
	QMutex mFrameQueueMutex;
	QQueue<AVFrame *> mFrameQueue;
	QMutex mPacketQueueMutex;
	QQueue<AVPacket *> mPacketQueue;

	Recorder *mOwner;
	PrivateClass( Recorder *recorder )
	{
		mCaptureActive = false;
		mOwner = recorder;
	}

	void HandleVideoFrame( IDeckLinkVideoInputFrame *videoFrame );
	void HandleAudioFrame( IDeckLinkAudioInputPacket *audioFrame );
	void FillVideoFrame( AVFrame *src );

	bool InitVideoDecoder( AVCodecID inputCodecID, AVPixelFormat inputPixelFormat );
	bool AddAudioStream( AVCodecID codec_id );
	bool AddVideoStream( AVCodecID codec_id );
	bool DecodeAndEnqueue( AVPacket *pkt );
	bool EncodeAndEnqueueFrame( AVFrame *frame );
	void Flush( AVCodecContext *codecContext, int streamIndex );
	int InterleaveFrameIntoFile( AVPacket *packet );
	void DecodingThreadFunction();
	void EncodingThreadFunction();
	void PacketWritingThreadFunction();

	bool ShouldEncoderKeepRunning() const;
	bool ShouldWriterKeepRunning() const;
};

void Recorder::PrivateClass::HandleVideoFrame( IDeckLinkVideoInputFrame *videoFrame )
{
	if ( !videoFrame )
	{
		return;
	}

	if ( videoFrame->GetFlags() & bmdFrameHasNoInputSource )
	{
		fprintf( stdout, "Frame received (#%lu) - No input signal detected\n", mFrameCount );
		return;
	}
	else
	{
		fprintf( stdout, "Frame received (#%lu)\n", mFrameCount );
	}

	// get frame timing info (PTS & duration)
	BMDTimeValue frameTime;
	BMDTimeValue frameDuration;
	videoFrame->GetStreamTime( &frameTime, &frameDuration, mVideoStream->time_base.den );

	// get frame size & data
	long height = videoFrame->GetHeight();
	long width = videoFrame->GetWidth();
	Q_UNUSED( height )
	Q_UNUSED( width )
	void *frameBytes = nullptr;
	videoFrame->GetBytes( &frameBytes );

#if __BMD_TO_AVFRAME__
	AVFrame *frame = AllocateVideoFrame( AV_PIX_FMT_UYVY422, width, height );
	int ret = av_image_fill_arrays( frame->data, frame->linesize, ( const uint8_t * ) frameBytes, ( AVPixelFormat )frame->format, width, height, 32 );
	if ( ret < 0 )
	{
		char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
		fprintf( stderr, "Failed to fill frame arrays (%s)\n", errorString );
	}

	frame->pts = frame->pkt_dts = frameTime / mVideoStream->time_base.num;
	frame->pkt_duration = frameDuration;

	//fprintf( stdout, "Enqueue frame (%p), pts: %ld, duration %ld\n", ( void * )frame, frame->pts, frame->pkt_duration );
	mFrameQueueMutex.lock();
	mFrameQueue.enqueue( frame );
	mFrameQueueMutex.unlock();
#elif __BMD_TO_PACKET__
	AVPacket *pkt = av_packet_alloc();
	// set data info
	av_new_packet( pkt, videoFrame->GetRowBytes() * videoFrame->GetHeight() );
	memcpy( pkt->data, ( uint8_t * )frameBytes, pkt->size );
	// set timing
	pkt->dts = pkt->pts = frameTime / mVideoStream->time_base.num;
	pkt->duration = frameDuration;
	// other packet settings
	pkt->flags |= AV_PKT_FLAG_KEY;
	pkt->stream_index = mVideoStream->index;

	//fprintf(stdout, "Enqueue packet %p with dts %ld, pts %ld duration %ld\n", (void*)pkt, pkt->dts, pkt->pts, pkt->duration);
	mDecodePacketQueueMutex.lock();
	mDecodePacketQueue.enqueue( pkt );
	mDecodePacketQueueMutex.unlock();
#endif
}

void Recorder::PrivateClass::HandleAudioFrame( IDeckLinkAudioInputPacket */*audioFrame*/ )
{
	// do not record audio (maybe later)
}

void Recorder::PrivateClass::FillVideoFrame( AVFrame *src )
{
	if ( !src )
	{
		return;
	}

	if ( !mVideoEncodingFrame )
	{
		return;
	}

	/* as we get AV_PIX_FMT_UYVY422 picture, we must convert it to the codec pixel format if needed */
	mSwScaleContext = sws_getCachedContext( mSwScaleContext,
											src->width, src->height, ( AVPixelFormat )src->format,
											mVideoEncodingFrame->width, mVideoEncodingFrame->height, ( AVPixelFormat )mVideoEncodingFrame->format,
											SWS_BICUBIC, NULL, NULL, NULL );
	if ( !mSwScaleContext )
	{
		fprintf( stderr, "Could not initialize the conversion context (SwsContext) to chane pixel format\n" );
		return;
	}

	int ret = sws_scale( mSwScaleContext,
						 src->data, src->linesize, 0, src->height,
						 mVideoEncodingFrame->data, mVideoEncodingFrame->linesize );
	if ( ret < 0 )
	{
		char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
		fprintf( stderr, "Failed to convert frame. sws_scale error: %s'n", errorString );
	}

	av_frame_copy_props( mVideoEncodingFrame, src );
}

bool Recorder::PrivateClass::InitVideoDecoder( AVCodecID inputCodecId, AVPixelFormat inputPixelFormat )
{
	const AVCodec *codec = avcodec_find_decoder( inputCodecId );
	if ( !codec )
	{
		fprintf( stderr, "Decoder codec not found\n" );
		return false;
	}

	mVideoDecodingContext = avcodec_alloc_context3( codec );
	if ( !mVideoDecodingContext )
	{
		fprintf( stderr, "Could not alloc an decoding context\n" );
		return false;
	}

	mVideoDecodingContext->codec_id = inputCodecId;
	mVideoDecodingContext->codec_type = AVMEDIA_TYPE_VIDEO;

	mVideoDecodingContext->width = mVideoWidth;
	mVideoDecodingContext->height = mVideoHeight;
	mVideoDecodingContext->time_base = mTimeBase;
	mVideoDecodingContext->pix_fmt = inputPixelFormat;

	//videoCodecContext->thread_type = FF_THREAD_SLICE;
	//mVideoDecodingContext->thread_count = 8;

	// open the codec
	if ( avcodec_open2( mVideoDecodingContext, codec, nullptr /*dict*/ ) < 0 )
	{
		fprintf( stderr, "could not open decoder codec\n" );
		return false;
	}

	return true;
}

bool Recorder::PrivateClass::AddAudioStream( AVCodecID codec_id )
{
	const AVCodec *codec = avcodec_find_encoder( codec_id );
	if ( !codec )
	{
		fprintf( stderr, "Audio codec not found\n" );
		return false;
	}

	mAudioCodecContext = avcodec_alloc_context3( codec );
	if ( !mAudioCodecContext )
	{
		fprintf( stderr, "Could not alloc an encoding context\n" );
		return false;
	}

	mAudioCodecContext->codec_id = codec_id;
	mAudioCodecContext->codec_type = AVMEDIA_TYPE_AUDIO;

	mAudioCodecContext->sample_fmt = AV_SAMPLE_FMT_S16;
	mAudioCodecContext->sample_rate = 48000;
	mAudioCodecContext->channels = 2;
	// some formats want stream headers to be separate
	if ( mFormatContext->oformat->flags & AVFMT_GLOBALHEADER )
	{
		mAudioCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	mAudioStream = avformat_new_stream( mFormatContext, NULL );
	if ( !mAudioStream )
	{
		fprintf( stderr, "Could not alloc audio stream\n" );
		return false;
	}
	mAudioStream->id = mFormatContext->nb_streams - 1;

	if ( avcodec_open2( mAudioCodecContext, codec, NULL ) < 0 )
	{
		fprintf( stderr, "could not open audio codec\n" );
		return false;
	}

	int ret = avcodec_parameters_from_context( mAudioStream->codecpar, mAudioCodecContext );
	if ( ret < 0 )
	{
		fprintf( stderr, "Could not copy the audio stream parameters\n" );
		return false;
	}

	return true;
}

bool Recorder::PrivateClass::AddVideoStream( AVCodecID codec_id )
{
	const AVCodec *codec = avcodec_find_encoder( codec_id );
	if ( !codec )
	{
		fprintf( stderr, "Video codec not found\n" );
		return false;
	}

	mVideoCodecContext = avcodec_alloc_context3( codec );
	if ( !mVideoCodecContext )
	{
		fprintf( stderr, "Could not alloc an encoding context\n" );
		return false;
	}

	mVideoCodecContext->codec_id = codec_id;
	mVideoCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;

	mVideoCodecContext->width = mVideoWidth;
	mVideoCodecContext->height = mVideoHeight;
	mVideoCodecContext->time_base = mTimeBase;
	mVideoCodecContext->pix_fmt = mPixelFormat;

	if ( mVideoCodecContext->codec_id == AV_CODEC_ID_PRORES )
	{
		mVideoCodecContext->profile            = FF_PROFILE_PRORES_LT;
	}
	else if ( mVideoCodecContext->codec_id == AV_CODEC_ID_H264 )
	{
		mVideoCodecContext->gop_size           = 1;
		mVideoCodecContext->keyint_min         = 1;
		mVideoCodecContext->profile            = FF_PROFILE_H264_HIGH;
	}
	if ( mVideoCodecContext->codec_id == AV_CODEC_ID_PRORES )
	{
		av_opt_set( mVideoCodecContext->priv_data, "profile", qUtf8Printable( QString::number( mVideoCodecContext->profile ) ), 0 );
	}
	mVideoCodecContext->thread_type = FF_THREAD_FRAME;
	mVideoCodecContext->thread_count = 4;

	// some formats want stream headers to be separate
	if ( mFormatContext->oformat->flags & AVFMT_GLOBALHEADER )
	{
		mVideoCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	// create stream
	mVideoStream = avformat_new_stream( mFormatContext, NULL );
	if ( !mVideoStream )
	{
		fprintf( stderr, "Could not alloc video stream\n" );
		return false;
	}
	mVideoStream->id = mFormatContext->nb_streams - 1;

	// open the codec
	if ( avcodec_open2( mVideoCodecContext, codec, nullptr /*dict*/ ) < 0 )
	{
		fprintf( stderr, "Could not open video codec\n" );
		return false;
	}

	// copy codec params to stream
	int ret = avcodec_parameters_from_context( mVideoStream->codecpar, mVideoCodecContext );
	if ( ret < 0 )
	{
		fprintf( stderr, "Could not copy the video stream parameters\n" );
		return false;
	}

	// allocate frame for encoding
	mVideoEncodingFrame = AllocateVideoFrame( mPixelFormat, mVideoWidth, mVideoHeight );

	return true;
}

bool Recorder::PrivateClass::DecodeAndEnqueue( AVPacket *pkt )
{
	//fprintf(stdout, "Decoding packet %p with dts %ld, pts %ld\n", (void*)pkt, pkt->dts, pkt->pts);
	int ret = avcodec_send_packet( mVideoDecodingContext, av_packet_clone( pkt ) );
	if ( ret < 0 )
	{
		char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
		fprintf( stderr, "Error send packet for decoding: %s\n", errorString );
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	while ( ret >= 0 )
	{
		ret = avcodec_receive_frame( mVideoDecodingContext, frame );
		if ( ret == AVERROR( EAGAIN ) || ret == AVERROR_EOF )
		{
			av_frame_free( &frame );
			return true;
		}
		else if ( ret < 0 )
		{
			char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
			fprintf( stderr, "Error during decoding: %s\n", errorString );
			av_frame_free( &frame );
			return false;
		}

		//fprintf(stdout, "Decoded frame pts %ld dts %ld width %d height %d\n", frame->pts, frame->pkt_dts, frame->width, frame->height);
		mFrameQueueMutex.lock();
		mFrameQueue.enqueue( av_frame_clone( frame ) );
		mFrameQueueMutex.unlock();
	}

	// maybe this is missing
	// if (ret == AVERROR_EOF) {
	// 	avcodec_flush_buffers( mVideoDecodingContext );
	// }

	return true;
}

bool Recorder::PrivateClass::EncodeAndEnqueueFrame( AVFrame *frame )
{
	int streamIndex = -1;
	AVCodecContext *codecContext = nullptr;
	AVFrame *encodingFrame = nullptr;
	if ( frame->width > 0 && frame->height > 0 )
	{
		streamIndex = mVideoStream->index;
		codecContext = mVideoCodecContext;
		encodingFrame = mVideoEncodingFrame;
		FillVideoFrame( frame );

		//	if ( mVideoCodecContext->flags & ( AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME ) )
		//	{
		//		mVideoEncodingFrame->top_field_first = 0; // !!ost->top_field_first;
		//	}
		if ( mVideoEncodingFrame->interlaced_frame )
		{
			if ( mVideoCodecContext->codec->id == AV_CODEC_ID_MJPEG )
			{
				mVideoStream->codecpar->field_order = mVideoEncodingFrame->top_field_first ? AV_FIELD_TT : AV_FIELD_BB;
			}
			else
			{
				mVideoStream->codecpar->field_order = mVideoEncodingFrame->top_field_first ? AV_FIELD_TB : AV_FIELD_BT;
			}
		}
		else
		{
			mVideoStream->codecpar->field_order = AV_FIELD_PROGRESSIVE;
		}
		mVideoEncodingFrame->quality = mVideoCodecContext->global_quality;
		mVideoEncodingFrame->pict_type = AV_PICTURE_TYPE_NONE;
		mVideoEncodingFrame->time_base = mVideoCodecContext->time_base;
	}
	if ( frame->sample_rate > 0 )
	{
		streamIndex = mAudioStream->index;
		codecContext = mAudioCodecContext;
	}

	//fprintf( stdout, "Encode frame pts: %ld dts: %ld, duration: %ld\n", encodingFrame->pts, encodingFrame->pkt_dts, encodingFrame->pkt_duration );
	int ret = avcodec_send_frame( codecContext, av_frame_clone( encodingFrame ) );
	if ( ret < 0 )
	{
		char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
		fprintf( stderr, "Error send frame for encoding: %s\n", errorString );
		return false;
	}

	AVPacket *pkt = av_packet_alloc();
	//deprecated: av_init_packet( pkt );
	while ( ret >= 0 )
	{
		ret = avcodec_receive_packet( mVideoCodecContext, pkt );
		if ( ret == AVERROR( EAGAIN ) || ret == AVERROR_EOF )
		{
			av_packet_free( &pkt );
			return true;
		}
		else if ( ret < 0 )
		{
			char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
			av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
			fprintf( stderr, "Error during encoding: %s\n", errorString );
			av_packet_free( &pkt );
			return false;
		}

		pkt->stream_index = streamIndex;
		pkt->dts = pkt->pts = frame->pts;
		pkt->duration = frame->pkt_duration;

		//fprintf( stdout, "Enqueue packet %p pts: %ld dts: %ld with buffer %p\n", ( void * )pkt, pkt->pts, pkt->dts, ( void * )pkt->buf );
		mPacketQueueMutex.lock();
		mPacketQueue.enqueue( av_packet_clone( pkt ) );
		mPacketQueueMutex.unlock();
	}

	return true;
}

void Recorder::PrivateClass::Flush( AVCodecContext *codecContext, int streamIndex )
{
	AVPacket *encodedPacket = av_packet_alloc();
	// deprecated: av_init_packet( encodedPacket );
	encodedPacket->data = nullptr;
	encodedPacket->size = 0;

	int ret = avcodec_send_frame( codecContext, nullptr );
	if ( ret != 0 )
	{
		fprintf( stderr, "avcode_send_frame failed with NULL frame: %d\n", ret );
	}
	while ( ret >= 0 )
	{
		ret = avcodec_receive_packet( codecContext, encodedPacket );
		if ( ret == AVERROR( EAGAIN ) || ret == AVERROR_EOF )
		{
			break;
		}
		if ( ret == 0 || ret == 1 )
		{
			encodedPacket->stream_index = streamIndex;
			encodedPacket->dts = encodedPacket->pts;
			//fprintf( stdout, "Enqueue flushing packet %p pts: %ld dts: %ld with buffer %p\n", ( void * )encodedPacket, encodedPacket->pts, encodedPacket->dts, ( void * )encodedPacket->buf );
			mPacketQueueMutex.lock();
			mPacketQueue.enqueue( av_packet_clone( encodedPacket ) );
			mPacketQueueMutex.unlock();
		}
	}
	// one time memory leak: av_packet_free( &encodedPacket );
}

int Recorder::PrivateClass::InterleaveFrameIntoFile( AVPacket *packet )
{
	if ( packet == nullptr || mFormatContext == nullptr )
	{
		return -1;
	}

	return av_interleaved_write_frame( mFormatContext, packet );
}

void Recorder::PrivateClass::DecodingThreadFunction()
{
	bool empty = true;
	AVPacket *pkt;
	for ( ;; )
	{
		pkt = nullptr;
		{
			QMutexLocker locker( &mDecodePacketQueueMutex );
			empty = mDecodePacketQueue.isEmpty();
			if ( !empty )
			{
				pkt = mDecodePacketQueue.dequeue();
			}
		}

		if ( empty )
		{
			if ( !mCaptureActive )
			{
				break;
			}
			QThread::msleep( 20 );
			continue;
		}

		if ( pkt != nullptr )
		{
			DecodeAndEnqueue( pkt );
			av_packet_free( &pkt );
		}
	}
}

void Recorder::PrivateClass::EncodingThreadFunction()
{
	bool empty = true;
	AVFrame *frame = nullptr;
	for ( ;; )
	{
		frame = nullptr;
		{
			QMutexLocker locker( &mFrameQueueMutex );
			empty = mFrameQueue.isEmpty();
			if ( !empty )
			{
				frame = mFrameQueue.dequeue();
			}
		}

		if ( empty )
		{
			if ( !ShouldEncoderKeepRunning() )
			{
				break;
			}
			QThread::msleep( 20 );
			continue;
		}

		if ( frame )
		{
			EncodeAndEnqueueFrame( frame );
			av_frame_free( &frame );
		}
	}

	Flush( mVideoCodecContext, mVideoStream->index );
	Flush( mAudioCodecContext, mAudioStream->index );
}

void Recorder::PrivateClass::PacketWritingThreadFunction()
{
	AVPacket *packet = nullptr;

	while ( true )
	{
		// mRunPacketWritingThread is not sufficient because when recording was stopped it's still necessary to write all queued frames
		if ( !ShouldWriterKeepRunning() )
		{
			bool empty = true;

			mPacketQueueMutex.lock();
			empty = mPacketQueue.isEmpty();
			mPacketQueueMutex.unlock();

			if ( empty )
			{
				// empty buffers => feel free to end
				break;
			}
		}

		mPacketQueueMutex.lock();
		if ( mPacketQueue.isEmpty() == false )
		{
			packet = mPacketQueue.dequeue();
		}
		mPacketQueueMutex.unlock();

		if ( packet )
		{
			fprintf( stdout, "Write packet %p pts: %ld dts: %ld with buffer %p\n", ( void * )packet, packet->pts, packet->dts, ( void * )packet->buf );
			InterleaveFrameIntoFile( packet );
			packet = nullptr; // interleave write takes ownership of packet
		}
	}

	mOwner->CleanUp();
	qApp->quit();
}

bool Recorder::PrivateClass::ShouldEncoderKeepRunning() const
{
	if ( mCaptureActive )
	{
		return true;
	}
	if ( mDecodingThread.isRunning() )
	{
		return true;
	}
	return false;
}

bool Recorder::PrivateClass::ShouldWriterKeepRunning() const
{
	if ( mCaptureActive )
	{
		return true;
	}
	if ( mEncodingThread.isRunning() )
	{
		return true;
	}
	return false;
}

///@endcond INTERNAL

Recorder::Recorder()
{
	d = new Recorder::PrivateClass( this );
}

Recorder::~Recorder()
{
	delete d;
	d = nullptr;
}

HRESULT Recorder::VideoInputFrameArrived( IDeckLinkVideoInputFrame *videoFrame, IDeckLinkAudioInputPacket *audioFrame )
{
	d->mFrameCount++;

	if ( d->mFrameCount > 500 )
	{
		d->mCaptureActive = false;
	}

	if ( d->mCaptureActive )
	{
		// Handle Video Frame
		d->HandleVideoFrame( videoFrame );

		// Handle Audio Frame
		d->HandleAudioFrame( audioFrame );
	}

	return S_OK;
}


bool Recorder::Init( int timeBaseNum, int timeBaseDen )
{
	d->mTimeBase = {timeBaseNum, timeBaseDen};

	if ( !d->mOutputFormat )
	{
		d->mOutputFormat = av_guess_format( nullptr, VIDEO_OUTPUT_FILE, nullptr );
		if ( !d->mOutputFormat )
		{
			fprintf( stderr, "Unable to guess output format\n" );
			return false;
		}
	}

	avformat_alloc_output_context2( &d->mFormatContext, d->mOutputFormat, nullptr, VIDEO_OUTPUT_FILE );
	if ( !d->mFormatContext )
	{
		printf( "Could not deduce output format from file extension.\n" );
		return false;
	}

	bool ok = true;
	ok &= d->InitVideoDecoder( d->mInputVideoCodec, d->mInputPixelFormat );
	ok &= d->AddVideoStream( d->mVideoCodec );
	ok &= d->AddAudioStream( d->mAudioCodec );
	if ( !ok )
	{
		fprintf( stderr, "Failed to add streams\n" );
		return false;
	}

	if ( !( d->mOutputFormat->flags & AVFMT_NOFILE ) )
	{
		if ( avio_open( &d->mFormatContext->pb, d->mFormatContext->url, AVIO_FLAG_WRITE ) < 0 )
		{
			fprintf( stderr, "Could not open '%s'\n", d->mFormatContext->url );
			return false;
		}
	}

	int ret = avformat_init_output( d->mFormatContext, nullptr );
	if ( ret < 0 )
	{
		char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
		fprintf( stderr, "Failed to init output '%s'\n", errorString );
	}
	ret = avformat_write_header( d->mFormatContext, nullptr );
	if ( ret < 0 )
	{
		char errorString[AV_ERROR_MAX_STRING_SIZE] = {0};
		av_make_error_string( errorString, AV_ERROR_MAX_STRING_SIZE, ret );
		fprintf( stderr, "Failed to write header'%s'\n", errorString );
	}

	return true;
}

void Recorder::Start()
{
	d->mCaptureActive = true;
	d->mDecodingThread = QtConcurrent::run( &d->mThreadPool, d, &Recorder::PrivateClass::DecodingThreadFunction );
	d->mEncodingThread = QtConcurrent::run( &d->mThreadPool, d, &Recorder::PrivateClass::EncodingThreadFunction );
	d->mFileWritingThread = QtConcurrent::run( &d->mThreadPool, d, &Recorder::PrivateClass::PacketWritingThreadFunction );
}

void Recorder::Stop()
{
	d->mCaptureActive = false;
	while ( d->mEncodingThread.isRunning() || d->mFileWritingThread.isRunning() )
	{
		QThread::msleep( 50 );
	}
}

void Recorder::CleanUp()
{
	if ( d->mFormatContext != nullptr )
	{
		int ret = av_write_trailer( d->mFormatContext );
		if ( ret < 0 )
		{
			fprintf( stderr, "Error occured while writing trailer of %s. Errno: %d\n", d->mFormatContext->url, ret );
		}

		if ( d->mOutputFormat != nullptr  && !( d->mOutputFormat->flags & AVFMT_NOFILE ) )
		{
			/* close the output file */
			avio_close( d->mFormatContext->pb );
		}

		/* free the stream */
		avformat_free_context( d->mFormatContext );
		d->mFormatContext = nullptr;
	}
}
