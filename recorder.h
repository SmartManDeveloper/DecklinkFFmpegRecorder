#ifndef RECORDER_H
#define RECORDER_H

#include <pthread.h>
#include "decklink/DeckLinkAPI.h"

class Recorder : public IDeckLinkInputCallback
{
public:
	Recorder();
	~Recorder();

	bool Init( int timeBaseNum, int timeBaseDen );
	void Start();
	void Stop();
	void CleanUp();

public:
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID /*iid*/, LPVOID */*ppv*/ ) override
	{
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef( void ) override
	{
		pthread_mutex_lock( &mMutex );
		mRefCount++;
		pthread_mutex_unlock( &mMutex );

		return ( ULONG )mRefCount;
	}
	ULONG STDMETHODCALLTYPE  Release( void ) override
	{
		pthread_mutex_lock( &mMutex );
		mRefCount--;
		pthread_mutex_unlock( &mMutex );

		if ( mRefCount == 0 )
		{
			delete this;
			return 0;
		}

		return ( ULONG )mRefCount;
	}
	HRESULT STDMETHODCALLTYPE VideoInputFormatChanged( BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode *, BMDDetectedVideoInputFormatFlags ) override
	{
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE VideoInputFrameArrived( IDeckLinkVideoInputFrame *, IDeckLinkAudioInputPacket * ) override;

private:
	ULONG mRefCount;
	pthread_mutex_t mMutex;

	class PrivateClass;
	PrivateClass *d;
};

#endif // RECORDER_H
