#ifndef _INCLUDE_ACCELERATOR_WINDOWS_EXCEPTION_POLICY_H_
#define _INCLUDE_ACCELERATOR_WINDOWS_EXCEPTION_POLICY_H_

#include <atomic>
#include <cstdint>

// Pure policy so the exception selection can be tested without a Windows SDK.
// First chance is needed for engines which intercept faults before Breakpad's
// unhandled filter. It cannot tell whether the application will later recover.
inline bool ShouldCaptureWindowsException(uint32_t code, uint32_t flags, bool firstChance)
{
	if (code == 0) {
		return (flags & 1u) != 0; // Valve Sys_Error, EXCEPTION_NONCONTINUABLE
	}
	if (!firstChance) {
		return false;
	}
	switch (code) {
		case 0xC0000005u: // EXCEPTION_ACCESS_VIOLATION
		case 0xC0000008u: // EXCEPTION_INVALID_HANDLE
		case 0xC000008Cu: // EXCEPTION_ARRAY_BOUNDS_EXCEEDED
		case 0x80000002u: // EXCEPTION_DATATYPE_MISALIGNMENT
		case 0xC000001Du: // EXCEPTION_ILLEGAL_INSTRUCTION
		case 0xC0000094u: // EXCEPTION_INT_DIVIDE_BY_ZERO
		case 0xC00000FDu: // EXCEPTION_STACK_OVERFLOW
		case 0xC0000409u: // STATUS_STACK_BUFFER_OVERRUN / fail-fast, if dispatched
		case 0xC0000374u: // STATUS_HEAP_CORRUPTION, if dispatched
			return true;
		default:
			return false; // Includes breakpoints, single-step, and C++ throws.
	}
}

class WindowsCaptureGate {
public:
	static_assert(std::atomic_bool::is_always_lock_free, "Windows crash-capture gate must be lock-free");
	void Reset() {
		m_busy.store(false);
		m_stopping.store(false);
	}
	bool TryBegin() {
		if (m_stopping.load()) {
			return false;
		}
		bool expected = false;
		if (!m_busy.compare_exchange_strong(expected, true)) {
			return false;
		}
		if (m_stopping.load()) {
			m_busy.store(false);
			return false;
		}
		return true;
	}
	void Finish() { m_busy.store(false); }
	void RequestStop() { m_stopping.store(true); }
	bool IsBusy() const { return m_busy.load(); }

private:
	std::atomic_bool m_busy{false};
	std::atomic_bool m_stopping{false};
};

#endif
