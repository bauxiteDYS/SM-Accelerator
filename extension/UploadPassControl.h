#ifndef _INCLUDE_ACCELERATOR_UPLOAD_PASS_CONTROL_H_
#define _INCLUDE_ACCELERATOR_UPLOAD_PASS_CONTROL_H_

#include <atomic>

enum class UploadRequestResult { NotStarted, Succeeded, Failed };

// Only RequestStop is called from another thread. Reset is called before the
// worker starts; all remaining operations belong to the upload worker.
class UploadPassControl {
public:
	void Reset() {
		m_failed = false;
		m_stopRequested.store(false);
	}
	void RequestStop() { m_stopRequested.store(true); }
	bool HasFailed() const { return m_failed; }
	bool ShouldStop() const { return m_failed || m_stopRequested.load(); }
	void RecordFailure() { m_failed = true; }

	template <typename Request>
	UploadRequestResult RunRequest(Request request) {
		if (ShouldStop()) {
			return UploadRequestResult::NotStarted;
		}
		// Cooperative only: a stop racing this check can leave this request
		// in flight. Never abandon or forcibly terminate it.
		if (!request()) {
			RecordFailure();
			return UploadRequestResult::Failed;
		}
		return UploadRequestResult::Succeeded;
	}

private:
	std::atomic_bool m_stopRequested{false};
	bool m_failed = false;
};

#endif
