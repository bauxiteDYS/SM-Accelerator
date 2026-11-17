// Minimal test double for the public SourceMod transfer-handler interface.
#pragma once
#include <cstddef>
namespace SourceMod {
class IWebTransfer {};
enum DownloadWriteStatus { DownloadWrite_Okay, DownloadWrite_Error };
class ITransferHandler {
public:
	virtual DownloadWriteStatus OnDownloadWrite(IWebTransfer *, void *, void *, size_t, size_t) {
		return DownloadWrite_Error;
	}
};
}
