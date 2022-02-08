#ifndef DECKLINKMANAGER_H
#define DECKLINKMANAGER_H

class IDeckLinkInputCallback;
class DecklinkManager
{
public:
	DecklinkManager( IDeckLinkInputCallback *delegate );
	~DecklinkManager();

	bool Init();
	bool Start();
	bool Stop();
	void CleanUp();

	bool GetTimeBase( int &num, int &den );

private:
	class PrivateClass;
	PrivateClass *d;
};

#endif // DECKLINKMANAGER_H
