#ifndef _INCLUDE_FORWARDS_H_
#define _INCLUDE_FORWARDS_H_

namespace extforwards
{
	// Initialize the sourcepawn forwards.
	void Init();
	// Shutdown the sourcepawn forwards.
	void Shutdown();
	// Main-thread only. Ready after Init and until Shutdown.
	bool IsInitialized();
	// Calls the completion forward synchronously on the main thread.
	void CallOnDoneUploadingForward();
}



#endif // !_INCLUDE_FORWARDS_H_
