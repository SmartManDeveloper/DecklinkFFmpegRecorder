#include "decklinkmanager.h"

#include <stdio.h>

#include "decklink/DeckLinkAPI.h"

///@cond INTERNAL

class DecklinkManager::PrivateClass
{
public:
	uint8_t mCameraIndex = 0;

	int mAudioChannelsCount = 2;
	int mAudioSampleDepth = 16;
	BMDDisplayMode mDesiredDisplayMode = bmdModeHD1080p50;

	IDeckLinkIterator *mDeckLinkIterator = nullptr;
	IDeckLink *mDeckLink = nullptr;
	IDeckLinkInput *mDeckLinkInput = nullptr;
	//IDeckLinkConfiguration *mDeckLinkConfiguration = nullptr;
	IDeckLinkDisplayMode *mDecklinkDisplayMode = nullptr;

	IDeckLinkInputCallback *mDelegate;

	PrivateClass( IDeckLinkInputCallback *delegate );

	void SetupDecklinkConnections();
	bool GetDisplayMode();
};

DecklinkManager::PrivateClass::PrivateClass( IDeckLinkInputCallback *delegate )
{
	this->mDelegate = delegate;
}

void DecklinkManager::PrivateClass::SetupDecklinkConnections()
{
	//	result = S_OK;
	//	switch ( aconnection )
	//	{
	//		case 1:
	//			result = DECKLINK_SET_AUDIO_CONNECTION( bmdAudioConnectionAnalog );
	//			break;
	//		case 2:
	//			result = DECKLINK_SET_AUDIO_CONNECTION( bmdAudioConnectionEmbedded );
	//			break;
	//		default:
	//			// do not change it
	//			break;
	//	}
	//	if ( result != S_OK )
	//	{
	//		fprintf( stderr, "Failed to set audio input - result = %08x\n", result );
	//		return false;
	//	}

	//	result = S_OK;
	//	switch ( vconnection )
	//	{
	//		case 1:
	//			result = DECKLINK_SET_VIDEO_CONNECTION( bmdVideoConnectionComposite );
	//			break;
	//		case 2:
	//			result = DECKLINK_SET_VIDEO_CONNECTION( bmdVideoConnectionComponent );
	//			break;
	//		case 3:
	//			result = DECKLINK_SET_VIDEO_CONNECTION( bmdVideoConnectionHDMI );
	//			break;
	//		case 4:
	//			result = DECKLINK_SET_VIDEO_CONNECTION( bmdVideoConnectionSDI );
	//			break;
	//		default:
	//			// do not change it
	//			break;
	//	}
	//	if ( result != S_OK )
	//	{
	//		fprintf( stderr, "Failed to set video input - result %08x\n", result );
	//		return false;
	//	}
}

bool DecklinkManager::PrivateClass::GetDisplayMode()
{
	// Obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output
	IDeckLinkDisplayModeIterator *displayModeIterator = nullptr;
	HRESULT result = mDeckLinkInput->GetDisplayModeIterator( &displayModeIterator );
	if ( result != S_OK )
	{
		fprintf( stderr, "Could not obtain the video output display mode iterator - result = %08x\n", result );
		if ( displayModeIterator != nullptr )
		{
			displayModeIterator->Release();
		}
		return false;
	}

	BMDDisplayMode displayModeID = bmdModeNTSC;
	IDeckLinkDisplayMode *displayMode = nullptr;
	while ( displayModeIterator->Next( &displayMode ) == S_OK )
	{
		displayModeID = displayMode->GetDisplayMode();
		if ( displayModeID == mDesiredDisplayMode )
		{
			mDecklinkDisplayMode = displayMode;
			break;
		}
		displayMode->Release();
	}

	if ( displayModeIterator != nullptr )
	{
		displayModeIterator->Release();
	}

	return ( mDecklinkDisplayMode != nullptr );
}

///@endcond INTERNAL

DecklinkManager::DecklinkManager( IDeckLinkInputCallback *delegate )
{
	d = new DecklinkManager::PrivateClass( delegate );
}

DecklinkManager::~DecklinkManager()
{
	delete d;
	d = nullptr;
}

bool DecklinkManager::Init()
{
	d->mDeckLinkIterator = CreateDeckLinkIteratorInstance();
	if ( !d->mDeckLinkIterator )
	{
		fprintf( stderr, "This application requires the DeckLink drivers installed.\n" );
		return false;
	}

	/* Connect to the DeckLink instance specified by cameraIndex*/
	HRESULT result = S_OK;
	uint8_t i = 0;
	do
	{
		result = d->mDeckLinkIterator->Next( &d->mDeckLink );
	}
	while ( i++ < d->mCameraIndex );

	if ( result != S_OK )
	{
		fprintf( stderr, "No DeckLink PCI cards found.\n" );
		return false;
	}

	if ( d->mDeckLink->QueryInterface( IID_IDeckLinkInput, ( void ** )&d->mDeckLinkInput ) != S_OK )
	{
		return false;
	}

	//result = d->deckLink->QueryInterface( IID_IDeckLinkConfiguration, ( void ** )&d->mDeckLinkConfiguration );
	//if ( result != S_OK )
	//{
	//	fprintf( stderr, "Could not obtain the IDeckLinkConfiguration interface - result = %08x\n", result );
	//	return false;
	//}

	d->SetupDecklinkConnections();

	bool displayModeOk = d->GetDisplayMode();
	if ( !displayModeOk )
	{
		return displayModeOk;
	}

	return true;
}

bool DecklinkManager::Start()
{
	d->mDeckLinkInput->SetCallback( d->mDelegate );

	HRESULT result = d->mDeckLinkInput->EnableVideoInput( d->mDesiredDisplayMode, bmdFormat8BitYUV, 0 );
	if ( result != S_OK )
	{
		fprintf( stderr, "Failed to enable video input. Is another application using the card?\n" );
		return false;
	}

	result = d->mDeckLinkInput->EnableAudioInput( bmdAudioSampleRate48kHz, d->mAudioSampleDepth, d->mAudioChannelsCount );
	if ( result != S_OK )
	{
		return false;
	}

	result = d->mDeckLinkInput->StartStreams();
	if ( result != S_OK )
	{
		return false;
	}

	return true;
}

bool DecklinkManager::Stop()
{
	d->mDeckLinkInput->SetCallback( nullptr );

	d->mDeckLinkInput->StopStreams(); // stop should be prior to DisableVideoInput/DisableAudioInput calls.
	d->mDeckLinkInput->DisableAudioInput();
	d->mDeckLinkInput->DisableVideoInput();
	if ( d->mDecklinkDisplayMode != nullptr )
	{
		d->mDecklinkDisplayMode->Release();
		d->mDecklinkDisplayMode = nullptr;
	}

	return true;
}

void DecklinkManager::CleanUp()
{
	if ( d->mDecklinkDisplayMode != nullptr )
	{
		d->mDecklinkDisplayMode->Release();
		d->mDecklinkDisplayMode = nullptr;
	}

	if ( d->mDeckLinkInput != nullptr )
	{
		d->mDeckLinkInput->Release();
		d->mDeckLinkInput = nullptr;
	}

	if ( d->mDeckLink != nullptr )
	{
		d->mDeckLink->Release();
		d->mDeckLink = nullptr;
	}

	if ( d->mDeckLinkIterator != nullptr )
	{
		d->mDeckLinkIterator->Release();
		d->mDeckLinkIterator = nullptr;
	}
}

bool DecklinkManager::GetTimeBase( int &num, int &den )
{
	BMDTimeValue frameRateDuration;
	BMDTimeScale frameRateScale;
	HRESULT result = d->mDecklinkDisplayMode->GetFrameRate( &frameRateDuration, &frameRateScale );
	num = frameRateDuration;
	den = frameRateScale;
	return ( result == S_OK );
}
