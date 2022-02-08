#include <QCoreApplication>

#include <unistd.h>

#include "decklink/DeckLinkAPI.h"
#include "decklinkmanager.h"
#include "recorder.h"

extern "C" {
#include "libavutil/log.h"
}

class MainApp
{
	DecklinkManager *mDecklinkManager = nullptr;
	Recorder *mRecorder = nullptr;

public:
	MainApp()
	{
		mRecorder = new Recorder();
		mRecorder->AddRef();
		mDecklinkManager = new DecklinkManager( mRecorder );
	}
	~MainApp()
	{
		delete mRecorder;
		mRecorder = nullptr;
		delete mDecklinkManager;
		mDecklinkManager = nullptr;
	}

	bool Init();
	void Start();
	void Stop();
	void CleanUp();

private:
	void _SetupDecklinkConnections();
	bool _CheckDisplayMode();
};

bool MainApp::Init()
{
	mDecklinkManager->Init();
	int num = 0, den = 1;
	mDecklinkManager->GetTimeBase( num, den );
	mRecorder->Init( num, den );
	return true;
}

void MainApp::Start()
{
	mDecklinkManager->Start();
	mRecorder->Start();
}

void MainApp::Stop()
{
	mDecklinkManager->Stop();
	mRecorder->Stop();
}

void MainApp::CleanUp()
{
	mDecklinkManager->CleanUp();
	mRecorder->CleanUp();
}

int main( int argc, char *argv[] )
{
	//av_log_set_level( AV_LOG_DEBUG );

	QCoreApplication a( argc, argv );

	MainApp *mainApp = new MainApp();

	bool ok = mainApp->Init();
	if ( !ok )
	{
		mainApp->CleanUp();
		return 1;
	}
	mainApp->Start();

	a.exec();

	mainApp->Stop();
	mainApp->CleanUp();
	delete mainApp;

	return 0;
}
