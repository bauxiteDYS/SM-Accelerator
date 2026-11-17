/*
 * =============================================================================
 * Accelerator Extension
 * Copyright (C) 2011 Asher Baker (asherkin).  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "extension.h"
#include "AcceleratorConfig.h"
#include "CrashUtils.h"
#include "DumpUploadQueue.h"
#include "UploadPassControl.h"
#include "WindowsExceptionPolicy.h"
#include <exception>

#ifndef PLATFORM_ARCH_FOLDER
#define PLATFORM_ARCH_FOLDER ""
#endif

#include <sp_vm_api.h>

#include "SourcePawnCompat.h"

#include <IWebternet.h>
#include <IGameHelpers.h>
#include "MemoryDownloader.h"
#include "forwards.h"
#include "natives.h"

#if defined _LINUX
#include "client/linux/handler/exception_handler.h"
#include "common/linux/linux_libc_support.h"
#include "third_party/lss/linux_syscall_support.h"
#include "common/linux/dump_symbols.h"
#include "common/path_helper.h"

#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <paths.h>

class StderrInhibitor
{
	FILE *saved_stderr = nullptr;

public:
	StderrInhibitor() {
		saved_stderr = fdopen(dup(fileno(stderr)), "w");
		if (freopen(_PATH_DEVNULL, "w", stderr)) {
			// If it fails, not a lot we can (or should) do.
			// Add this brace section to silence gcc warnings.
		}
	}

	~StderrInhibitor() {
		fflush(stderr);
		dup2(fileno(saved_stderr), fileno(stderr));
		fclose(saved_stderr);
	}
};

// Taken from https://hg.mozilla.org/mozilla-central/file/3eb7623b5e63b37823d5e9c562d56e586604c823/build/unix/stdc%2B%2Bcompat/stdc%2B%2Bcompat.cpp
extern "C" void __attribute__((weak)) __cxa_throw_bad_array_new_length() {
	abort();
}

namespace std {
	/* We shouldn't be throwing exceptions at all, but it sadly turns out
	   we call STL (inline) functions that do. */
	void __attribute__((weak)) __throw_out_of_range_fmt(char const* fmt, ...) {
		va_list ap;
		char buf[1024];  // That should be big enough.

		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		buf[sizeof(buf) - 1] = 0;
		va_end(ap);

		__throw_range_error(buf);
	}
} // namespace std

// Updated versions of the SM ones for C++14
void operator delete(void *ptr, size_t sz) {
	free(ptr);
}

void operator delete[](void *ptr, size_t sz) {
	free(ptr);
}

#elif defined _WINDOWS
#define _STDINT // ~.~
#include "client/windows/handler/exception_handler.h"

#else
#error Bad platform.
#endif

#include <google_breakpad/processor/minidump.h>
#include <google_breakpad/processor/minidump_processor.h>
#include <google_breakpad/processor/process_state.h>
#include <google_breakpad/processor/call_stack.h>
#include <google_breakpad/processor/stack_frame.h>
#include <processor/pathname_stripper.h>

#include <sstream>
#include <streambuf>
#include <memory>

Accelerator g_accelerator;
SMEXT_LINK(&g_accelerator);

IWebternet *webternet;
IGameConfig *gameconfig;

static AcceleratorConfig acceleratorConfig;
static IGameHelpers *optionalGameHelpers = nullptr;

static bool AcquireGameHelpers()
{
	if (optionalGameHelpers) {
		return true;
	}
	SMInterface *iface = nullptr;
	if (!sharesys->RequestInterface(SMINTERFACE_GAMEHELPERS_NAME,
		SMINTERFACE_GAMEHELPERS_VERSION, myself, &iface) || !iface) {
		return false;
	}
	// Only use the SDK's declared interface, never reinterpret an older layout.
	optionalGameHelpers = static_cast<IGameHelpers *>(iface);
	return true;
}

typedef void (*GetSpew_t)(char *buffer, size_t length);
GetSpew_t GetSpew;
#if defined _WINDOWS
typedef void(__fastcall *GetSpewFastcall_t)(char *buffer, size_t length);
GetSpewFastcall_t GetSpewFastcall;
#endif

char spewBuffer[65536]; // Hi.

char crashMap[256];
char crashGamePath[512];
char crashCommandLine[1024];
char crashSourceModPath[512];
char crashGameDirectory[256];
char crashSourceModVersion[32];
char steamInf[1024];

char dumpStoragePath[512];
char logPath[512];

google_breakpad::ExceptionHandler *handler = NULL;

#if defined _LINUX
std::terminate_handler previousTerminateHandler = nullptr;

void terminateHandler()
{
	const char *msg = "missing exception";
	std::exception_ptr pEx = std::current_exception();
	if (pEx) {
		try {
			std::rethrow_exception(pEx);
		} catch(const std::exception &e) {
			msg = strdup(e.what());
		} catch(...) {
			msg = "unknown exception";
		}
	}

	size_t msgLength = strlen(msg) + 2;
	volatile char * volatile msgForCrashDumps = (char *)alloca(msgLength);
	strcpy((char *)msgForCrashDumps + 1, msg);

	abort();
}

void (*SignalHandler)(int, siginfo_t *, void *);

const int kExceptionSignals[] = {
	SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS
};

const int kNumHandledSignals = sizeof(kExceptionSignals) / sizeof(kExceptionSignals[0]);

static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	//printf("Wrote minidump to: %s\n", descriptor.path());

	if (succeeded) {
		sys_write(STDOUT_FILENO, "Wrote minidump to: ", 19);
	} else {
		sys_write(STDOUT_FILENO, "Failed to write minidump to: ", 29);
	}

	sys_write(STDOUT_FILENO, descriptor.path(), my_strlen(descriptor.path()));
	sys_write(STDOUT_FILENO, "\n", 1);

	if (!succeeded) {
		return succeeded;
	}

	char metadataPath[sizeof(dumpStoragePath) + 4];
	my_strlcpy(metadataPath, descriptor.path(), sizeof(metadataPath));
	my_strlcat(metadataPath, ".txt", sizeof(metadataPath));

	int extra = sys_open(metadataPath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (extra == -1) {
		sys_write(STDOUT_FILENO, "Failed to open metadata file!\n", 30);
		return succeeded;
	}

	sys_write(extra, "-------- CONFIG BEGIN --------", 30);
	sys_write(extra, "\nMap=", 5);
	sys_write(extra, crashMap, my_strlen(crashMap));
	sys_write(extra, "\nGamePath=", 10);
	sys_write(extra, crashGamePath, my_strlen(crashGamePath));
	sys_write(extra, "\nCommandLine=", 13);
	sys_write(extra, crashCommandLine, my_strlen(crashCommandLine));
	sys_write(extra, "\nSourceModPath=", 15);
	sys_write(extra, crashSourceModPath, my_strlen(crashSourceModPath));
	sys_write(extra, "\nGameDirectory=", 15);
	sys_write(extra, crashGameDirectory, my_strlen(crashGameDirectory));
	if (crashSourceModVersion[0]) {
		sys_write(extra, "\nSourceModVersion=", 18);
		sys_write(extra, crashSourceModVersion, my_strlen(crashSourceModVersion));
	}
	sys_write(extra, "\nExtensionVersion=", 18);
	sys_write(extra, SM_VERSION, my_strlen(SM_VERSION));
	sys_write(extra, "\nExtensionBuild=", 16);
	sys_write(extra, SM_BUILD_UNIQUEID, my_strlen(SM_BUILD_UNIQUEID));
	sys_write(extra, steamInf, my_strlen(steamInf));
	sys_write(extra, "\n-------- CONFIG END --------\n", 30);

	if (GetSpew) {
		GetSpew(spewBuffer, sizeof(spewBuffer));

		if (my_strlen(spewBuffer) > 0) {
			sys_write(extra, "-------- CONSOLE HISTORY BEGIN --------\n", 40);
			sys_write(extra, spewBuffer, my_strlen(spewBuffer));
			sys_write(extra, "-------- CONSOLE HISTORY END --------\n", 38);
		}
	}

	sys_close(extra);

	return succeeded;
}

void EnsureSignalHandlers()
{
	bool weHaveBeenFuckedOver = false;
	struct sigaction oact;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], NULL, &oact);

		if (oact.sa_sigaction != SignalHandler) {
			weHaveBeenFuckedOver = true;
			break;
		}
	}

	if (!weHaveBeenFuckedOver) {
		return;
	}

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaddset(&act.sa_mask, kExceptionSignals[i]);
	}

	act.sa_sigaction = SignalHandler;
	act.sa_flags = SA_ONSTACK | SA_SIGINFO;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], &act, NULL);
	}
}

#elif defined _WINDOWS
void *vectoredHandler = NULL;
static bool windowsFirstChance = true;
static WindowsCaptureGate windowsCaptureGate;
static std::atomic<DWORD> vectoredRequestThread{0};

LONG CALLBACK BreakpadVectoredHandler(_In_ PEXCEPTION_POINTERS ExceptionInfo)
{
	if (!ExceptionInfo || !ExceptionInfo->ExceptionRecord
		|| !ShouldCaptureWindowsException(ExceptionInfo->ExceptionRecord->ExceptionCode,
			ExceptionInfo->ExceptionRecord->ExceptionFlags, windowsFirstChance)
		|| !windowsCaptureGate.TryBegin()) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	// Capture selected faults before Valve can consume them or terminate.
	// The gate skips recursive/concurrent capture and lets unload wait for us.
	if (handler) {
		vectoredRequestThread.store(GetCurrentThreadId());
		const char message[] = "Accelerator: Windows exception intercepted; requesting minidump.\n";
		DWORD written;
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, nullptr);
		if (!handler->WriteMinidumpForException(ExceptionInfo)) {
			const char failure[] = "Accelerator: Breakpad did not complete dump writing.\n";
			WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failure, sizeof(failure) - 1, &written, nullptr);
		}
		vectoredRequestThread.store(0);
	}
	windowsCaptureGate.Finish();
	// Never delete a live handler or modify an application's exception record.
	// Leave the application's SEH/engine exception handling semantics intact.
	return EXCEPTION_CONTINUE_SEARCH;
}

static bool dumpCallback(const wchar_t* dump_path,
                         const wchar_t* minidump_id,
                         void* context,
                         EXCEPTION_POINTERS* exinfo,
                         MDRawAssertionInfo* assertion,
                         bool succeeded)
{
	if (!succeeded) {
		printf("Failed to write minidump to: %ls\\%ls.dmp\n", dump_path, minidump_id);
		return succeeded;
	}

	printf("Wrote minidump to: %ls\\%ls.dmp\n", dump_path, minidump_id);

	wchar_t metadataPath[1024];
	int length = _snwprintf_s(metadataPath, _countof(metadataPath), _TRUNCATE,
		L"%ls\\%ls.dmp.txt", dump_path, minidump_id);
	if (length < 0) {
		return succeeded;
	}
	FILE *extra = _wfopen(metadataPath, L"wb");
	if (!extra) {
		printf("Failed to open metadata file!\n");
		return succeeded;
	}

	fprintf(extra, "-------- CONFIG BEGIN --------");
	fprintf(extra, "\nMap=%s", crashMap);
	fprintf(extra, "\nGamePath=%s", crashGamePath);
	fprintf(extra, "\nCommandLine=%s", crashCommandLine);
	fprintf(extra, "\nSourceModPath=%s", crashSourceModPath);
	fprintf(extra, "\nGameDirectory=%s", crashGameDirectory);
	if (crashSourceModVersion[0]) {
		fprintf(extra, "\nSourceModVersion=%s", crashSourceModVersion);
	}
	fprintf(extra, "\nExtensionVersion=%s", SM_VERSION);
	fprintf(extra, "\nExtensionBuild=%s", SM_BUILD_UNIQUEID);
	const DWORD vectoredThread = vectoredRequestThread.load();
	const bool firstChance = handler && vectoredThread != 0
		&& vectoredThread == handler->get_requesting_thread_id();
	fprintf(extra, "\nFirstChanceCapture=%s", firstChance ? "yes" : "no");
	fprintf(extra, "%s", steamInf);
	fprintf(extra, "\n-------- CONFIG END --------\n");

	if (GetSpew || GetSpewFastcall) {
		if (GetSpew) {
			GetSpew(spewBuffer, sizeof(spewBuffer));
		} else if (GetSpewFastcall) {
			GetSpewFastcall(spewBuffer, sizeof(spewBuffer));
		}

		if (spewBuffer[0]) {
			fprintf(extra, "-------- CONSOLE HISTORY BEGIN --------\n%s-------- CONSOLE HISTORY END --------\n", spewBuffer);
		}
	}

	fclose(extra);

	return succeeded;
}

#else
#error Bad platform.
#endif

void OnGameFrame(bool simulating)
{
	// Only the main thread may execute SourcePawn forwards. Polling avoids
	// queued callbacks into extension code after an unload.
	g_accelerator.NotifyIfDoneUploading();
#if defined _LINUX
	EnsureSignalHandlers();
#endif
}

class ClogInhibitor
{
	std::streambuf *saved_clog = nullptr;

public:
	ClogInhibitor() {
		saved_clog = std::clog.rdbuf();
		std::clog.rdbuf(nullptr);
	}

	~ClogInhibitor() {
		std::clog.rdbuf(saved_clog);
	}
};

class UploadThread: public IThread
{
public:
	void Reset() { control.Reset(); }
	void RequestStop() { control.RequestStop(); }

private:
	UploadPassControl control;
	FILE *log = nullptr;
	char serverId[38] = "";

	void RemoveDump(const std::string &path, const std::string &metapath, const std::string &processedPath) {
		if (!metapath.empty()) {
			unlink(metapath.c_str());
		}
		// Keep the marker if removing the dump fails, to prevent re-uploading it.
		if (unlink(path.c_str()) == 0) {
			unlink(processedPath.c_str());
		}
	}

	void RunThread(IThreadHandle *pHandle) {
		rootconsole->ConsolePrint("Accelerator upload thread started.");

		const bool keepDumps = acceleratorConfig.GetBool("MinidumpKeepDumps", true);
		const char *uploadLimit = acceleratorConfig.GetValue("MinidumpUploadLimit");
		DumpUploadQueue queue(uploadLimit ? static_cast<size_t>(atoi(uploadLimit)) : 2);

		log = fopen(logPath, "a");
		if (!log) {
			g_pSM->LogError(myself, "Failed to open Accelerator log file: %s", logPath);
		}

		const std::string serverIdPath = std::string(dumpStoragePath) + "/server-id.txt";
		FILE *serverIdFile = fopen(serverIdPath.c_str(), "r");
		if (serverIdFile) {
			fread(serverId, 1, sizeof(serverId) - 1, serverIdFile);
			if (!feof(serverIdFile) || strlen(serverId) != 36) {
				serverId[0] = '\0';
			}
			fclose(serverIdFile);
		}
		if (!serverId[0]) {
			serverIdFile = fopen(serverIdPath.c_str(), "w");
			if (serverIdFile) {
				g_pSM->Format(serverId, sizeof(serverId), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
					rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, 0x40 | ((rand() % 255) & 0x0F), rand() % 255,
					0x80 | ((rand() % 255) & 0x3F), rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255);
				fputs(serverId, serverIdFile);
				fclose(serverIdFile);
			}
		}

		IDirectory *dumps = libsys->OpenDirectory(dumpStoragePath);
		if (!dumps) {
			g_pSM->LogError(myself, "Failed to open Accelerator dump directory: %s", dumpStoragePath);
			if (log) {
				fclose(log);
				log = nullptr;
			}
			g_accelerator.MarkAsDoneUploading();
			return;
		}

		int skip = 0;
		int count = 0;
		int failed = 0;
		size_t pending = 0;
		size_t attempted = 0;
		char presubmitToken[512];
		char response[512];

		while (!control.ShouldStop() && dumps->MoreFiles()) {
			if (!dumps->IsEntryFile()) {
				dumps->NextEntry();
				continue;
			}

			const char *name = dumps->GetEntryName();

			int namelen = strlen(name);
			if (namelen < 4 || strcmp(&name[namelen-4], ".dmp") != 0) {
				dumps->NextEntry();
				continue;
			}

			const std::string path = std::string(dumpStoragePath) + "/" + name;
			const std::string metapath = path + ".txt";
			const std::string processedPath = path + ".processed";

			// Markers survive restarts and also cover metadata-only uploads and
			// dumps that the crash service explicitly asked us not to upload.
			if (libsys->PathExists(processedPath.c_str())) {
				skip++;
				if (!keepDumps) {
					RemoveDump(path, metapath, processedPath);
				}
				dumps->NextEntry();
				continue;
			}

			++pending;
			time_t modified;
			if (libsys->FileTime(path.c_str(), FileTime_LastChange, &modified)) {
				queue.Consider(path, modified);
			} else {
				g_pSM->LogError(myself, "Cannot read dump modification time; leaving it on disk: %s", path.c_str());
			}
			dumps->NextEntry();
		}
		libsys->CloseDirectory(dumps);

		size_t deferred = pending - queue.Entries().size();
		size_t visited = 0;
		rootconsole->ConsolePrint("Accelerator selected %zu unprocessed dumps; %zu deferred.", queue.Entries().size(), deferred);
		if (log) fprintf(log, "Selected %zu unprocessed dumps; %zu deferred (limit or unreadable timestamp)\n", queue.Entries().size(), deferred);

		// Only this fixed selection is attempted. Failures and server-requested
		// skips consume a slot; never backfill from older dumps in the same run.
		for (const DumpCandidate &candidate : queue.Entries()) {
			if (control.ShouldStop()) {
				break;
			}
			++visited;
			const std::string &path = candidate.path;
			std::string metapath = path + ".txt";
			const std::string processedPath = path + ".processed";
			// Recheck in case files changed since the directory scan.
			if (libsys->PathExists(processedPath.c_str())) {
				++skip;
				if (!keepDumps) {
					RemoveDump(path, metapath, processedPath);
				}
				continue;
			}
			if (!libsys->IsPathFile(path.c_str())) {
				continue;
			}
			if (!libsys->PathExists(metapath.c_str())) {
				metapath.clear();
			}
			++attempted;
			presubmitToken[0] = '\0';
			PresubmitResponse presubmitResponse = kPRUploadCrashDumpAndMetadata;
			bool processed = false;

			bool canPresubmit = acceleratorConfig.GetBool("MinidumpPresubmit", true);

			if (canPresubmit) {
				presubmitResponse = PresubmitCrashDump(path.c_str(), presubmitToken, sizeof(presubmitToken));
			}

			switch (presubmitResponse) {
				case kPRLocalError:
					failed++;
					g_pSM->LogError(myself, "Accelerator failed to locally process crash dump");
					if (log) fprintf(log, "Failed to locally process crash dump");
					break;
				case kPRRemoteError:
					// Do not turn a failed presubmit into a larger upload.
					control.RecordFailure();
					failed++;
					g_pSM->LogError(myself, "Accelerator presubmit failed; stopping upload pass and keeping incomplete dumps.");
					break;
				case kPRStopped:
					if (control.HasFailed()) {
						failed++;
					}
					break;
				case kPRUploadCrashDumpAndMetadata:
				case kPRUploadMetadataOnly:
					if (UploadCrashDump((presubmitResponse == kPRUploadMetadataOnly) ? nullptr : path.c_str(), metapath.c_str(), presubmitToken, response, sizeof(response))) {
						processed = true;
						count++;
						g_pSM->LogError(myself, "Accelerator uploaded crash dump: %s", response);
						if (log) fprintf(log, "Uploaded crash dump: %s\n", response);
						UploadedCrash crash{ response };
						g_accelerator.StoreUploadedCrash(crash);
					} else {
						failed++;
						g_pSM->LogError(myself, "Accelerator failed to upload crash dump: %s", response);
						if (log) fprintf(log, "Failed to upload crash dump: %s\n", response);
					}
					break;
				case kPRDontUpload:
					processed = true;
					skip++;
					g_pSM->LogError(myself, "Accelerator crash dump upload skipped by server");
					if (log) fprintf(log, "Skipped due to server request\n");
					break;
			}

			// Retain incomplete work even with keep=no. A successful final
			// submission that returned during unload must still get its marker.
			if (!processed && control.ShouldStop()) {
				break;
			}

			if (processed) {
				// An empty sidecar is sufficient: only its existence matters.
				// Write it even when retention is off, in case deleting the dump fails.
				FILE *marker = fopen(processedPath.c_str(), "wb");
				if (marker) {
					fclose(marker);
				} else {
					g_pSM->LogError(myself, "Failed to mark processed crash dump %s; it may be submitted again on restart", path.c_str());
					if (log) fprintf(log, "Failed to create marker %s; dump may be submitted again on restart\n", processedPath.c_str());
				}
			}

			if (!keepDumps) {
				RemoveDump(path, metapath, processedPath);
			}

			if (log) fflush(log);
		}

		deferred += queue.Entries().size() - visited;
		if (control.ShouldStop()) {
			const char *reason = control.HasFailed() ? "a request/protocol failure" : "extension unload";
			rootconsole->ConsolePrint("Accelerator upload pass stopped after %s; incomplete dumps remain on disk.", reason);
			if (log) fprintf(log, "Upload pass stopped after %s; incomplete dumps remain on disk. A scan stopped early may not have counted every dump.\n", reason);
		}

		if (log) {
			fclose(log);
			log = nullptr;
		}

		g_accelerator.MarkAsDoneUploading();
		rootconsole->ConsolePrint("Accelerator upload thread finished. (%zu attempted, %d skipped, %d uploaded, %d failed, %zu deferred)", attempted, skip, count, failed, deferred);
	}

	void OnTerminate(IThreadHandle *pHandle, bool cancel) {
		rootconsole->ConsolePrint("Accelerator upload thread terminated. (canceled = %s)", (cancel ? "true" : "false"));
	}

#if defined _LINUX
	bool UploadSymbolFile(const google_breakpad::CodeModule *module, const char *presubmitToken) {
		if (control.ShouldStop()) {
			return false;
		}
		if (log) fprintf(log, "UploadSymbolFile\n");
		if (log) fflush(log);

		auto debugFile = module->debug_file();
		std::string vdsoOutputPath = "";

		if (false && debugFile == "linux-gate.so") {
			FILE *auxvFile = fopen("/proc/self/auxv", "rb");
			if (auxvFile) {
				char vdsoOutputPathBuffer[512];
				g_pSM->BuildPath(Path_SM, vdsoOutputPathBuffer, sizeof(vdsoOutputPathBuffer), "data/dumps/linux-gate.so");
				vdsoOutputPath = vdsoOutputPathBuffer;

				while (!feof(auxvFile)) {
					int auxvEntryId = 0;
					fread(&auxvEntryId, sizeof(auxvEntryId), 1, auxvFile);
					long auxvEntryValue = 0;
					fread(&auxvEntryValue, sizeof(auxvEntryValue), 1, auxvFile);

					if (auxvEntryId == 0) break;
					if (auxvEntryId != 33) continue; // AT_SYSINFO_EHDR

#ifdef PLATFORM_X64
					Elf64_Ehdr *vdsoHdr = (Elf64_Ehdr *)auxvEntryValue;
#else
					Elf32_Ehdr *vdsoHdr = (Elf32_Ehdr *)auxvEntryValue;
#endif
					auto vdsoSize = vdsoHdr->e_shoff + (vdsoHdr->e_shentsize * vdsoHdr->e_shnum);
					void *vdsoBuffer = malloc(vdsoSize);
					memcpy(vdsoBuffer, vdsoHdr, vdsoSize);

					FILE *vdsoFile = fopen(vdsoOutputPath.c_str(), "wb");
					if (vdsoFile) {
						fwrite(vdsoBuffer, 1, vdsoSize, vdsoFile);
						fclose(vdsoFile);
						debugFile = vdsoOutputPath;
					}

					free(vdsoBuffer);
					break;
				}

				fclose(auxvFile);
			}
		}

		if (debugFile[0] != '/') {
			return false;
		}

		if (log) fprintf(log, "Submitting symbols for %s\n", debugFile.c_str());
		if (log) fflush(log);

		auto debugFileDir = google_breakpad::DirName(debugFile);
		std::vector<std::string> debug_dirs{
			debugFileDir,
			debugFileDir + "/.debug",
			"/usr/lib/debug" + debugFileDir,
		};

		std::ostringstream outputStream;
		google_breakpad::DumpOptions options(ALL_SYMBOL_DATA, true, true, false);

		{
			StderrInhibitor stdrrInhibitor;

			if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", debug_dirs, options, outputStream)) {
				outputStream.str("");
				outputStream.clear();

				// Try again without debug dirs.
				if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", {}, options, outputStream)) {
					if (log) fprintf(log, "Failed to process symbol file\n");
					if (log) fflush(log);
					return false;
				}
			}
		}

		auto output = outputStream.str();
		// output = output.substr(0, output.find("\n"));
		// printf(">>> %s\n", output.c_str());

		if (debugFile == vdsoOutputPath) {
			unlink(vdsoOutputPath.c_str());
		}

		std::unique_ptr<IWebForm> form(webternet->CreateForm());
		if (!form) {
			return false;
		}

		const char *minidumpAccount = acceleratorConfig.GetValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		form->AddString("symbol_file", output.c_str());

		MemoryDownloader data;
		std::unique_ptr<IWebTransfer> xfer(webternet->CreateSession());
		if (!xfer) {
			return false;
		}
		xfer->SetFailOnHTTPError(true);

		const char *symbolUrl = acceleratorConfig.GetValue("MinidumpSymbolUrl");
		if (!symbolUrl) symbolUrl = "https://crash.limetech.org/symbols/submit";

		auto result = control.RunRequest([&]() {
			return xfer->PostAndDownload(symbolUrl, form.get(), &data, NULL);
		});

		if (result != UploadRequestResult::Succeeded) {
			if (log && result == UploadRequestResult::Failed) fprintf(log, "Symbol upload failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			if (log) fflush(log);
			return false;
		}

		int responseSize = data.GetSize();
		char *response = data.GetBuffer();
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		if (log) fprintf(log, "Symbol upload complete: %s\n", response);
		if (log) fflush(log);
		return true;
	}
#endif

	bool UploadModuleFile(const google_breakpad::CodeModule *module, const char *presubmitToken) {
		if (control.ShouldStop()) {
			return false;
		}
		const auto &codeFile = module->code_file();

#if !defined _WINDOWS
		if (codeFile[0] != '/') {
#else
		if (codeFile.size() < 3 || codeFile[1] != ':') {
#endif
			return false;
		}

		if (log) fprintf(log, "Submitting binary for %s\n", codeFile.c_str());
		if (log) fflush(log);

		std::unique_ptr<IWebForm> form(webternet->CreateForm());
		if (!form) {
			return false;
		}

		const char *minidumpAccount = acceleratorConfig.GetValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		form->AddString("debug_identifier", module->debug_identifier().c_str());
		form->AddString("code_identifier", module->code_identifier().c_str());

		form->AddFile("code_file", codeFile.c_str());

		MemoryDownloader data;
		std::unique_ptr<IWebTransfer> xfer(webternet->CreateSession());
		if (!xfer) {
			return false;
		}
		xfer->SetFailOnHTTPError(true);

		const char *binaryUrl = acceleratorConfig.GetValue("MinidumpBinaryUrl");
		if (!binaryUrl) binaryUrl = "https://crash.limetech.org/binary/submit";

		auto result = control.RunRequest([&]() {
			return xfer->PostAndDownload(binaryUrl, form.get(), &data, NULL);
		});

		if (result != UploadRequestResult::Succeeded) {
			if (log && result == UploadRequestResult::Failed) fprintf(log, "Binary upload failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			if (log) fflush(log);
			return false;
		}

		int responseSize = data.GetSize();
		char *response = data.GetBuffer();
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		if (log) fprintf(log, "Binary upload complete: %s\n", response);
		if (log) fflush(log);

		return true;
	}

	enum ModuleType {
		kMTUnknown,
		kMTSystem,
		kMTGame,
		kMTAddon,
		kMTExtension,
	};

	const char *ModuleTypeCode[5] = {
		"Unknown",
		"System",
		"Game",
		"Addon",
		"Extension",
	};

#if !defined _WINDOWS
#define PATH_SEP "/"
#else
#define PATH_SEP "\\"
#endif

	bool PathPrefixMatches(const std::string &prefix, const std::string &path) {
#if !defined _WINDOWS
		return strncmp(prefix.c_str(), path.c_str(), prefix.length()) == 0;
#else
		return _strnicmp(prefix.c_str(), path.c_str(), prefix.length()) == 0;
#endif
	}

	std::map<std::string, ModuleType, PathComparator> modulePathMap;
	bool InitModuleClassificationMap(const std::string &base) {
		if (!modulePathMap.empty()) {
			modulePathMap.clear();
		}

		modulePathMap[base] = kMTGame;
		modulePathMap[std::string(crashGamePath) + PATH_SEP "addons" PATH_SEP] = kMTAddon;
		modulePathMap[std::string(crashSourceModPath) + PATH_SEP "extensions" PATH_SEP] = kMTExtension;

		return true;
	}

	ModuleType ClassifyModule(const google_breakpad::CodeModule *module) {
		if (modulePathMap.empty()) {
			return kMTUnknown;
		}

		const auto &codeFile = module->code_file();

#if !defined _WINDOWS
		if (codeFile == "linux-gate.so") {
			return kMTSystem;
		}

		if (codeFile[0] != '/') {
#else
		if (codeFile.size() < 3 || codeFile[1] != ':') {
#endif
			return kMTUnknown;
		}

		for (decltype(modulePathMap)::const_iterator i = modulePathMap.begin(); i != modulePathMap.end(); ++i) {
			if (PathPrefixMatches(i->first, codeFile)) {
				return i->second;
			}
		}

		return kMTSystem;
	}

	std::string PathnameStripper_Directory(const std::string &path) {
		std::string::size_type slash = path.rfind('/');
		std::string::size_type backslash = path.rfind('\\');

		std::string::size_type file_start = 0;
		if (slash != std::string::npos && (backslash == std::string::npos || slash > backslash)) {
			file_start = slash + 1;
		} else if (backslash != std::string::npos) {
			file_start = backslash + 1;
		}

		return path.substr(0, file_start);
	}

	enum PresubmitResponse {
		kPRLocalError,
		kPRRemoteError,
		kPRStopped,
		kPRDontUpload,
		kPRUploadCrashDumpAndMetadata,
		kPRUploadMetadataOnly,
	};

	PresubmitResponse PresubmitCrashDump(const char *path, char *tokenBuffer, size_t tokenBufferLength) {
		if (control.ShouldStop()) {
			return kPRStopped;
		}
		google_breakpad::ProcessState processState;
		google_breakpad::ProcessResult processResult;
		google_breakpad::MinidumpProcessor minidumpProcessor(nullptr, nullptr);

		{
			ClogInhibitor clogInhibitor;
			processResult = minidumpProcessor.Process(path, &processState);
		}

		if (processResult != google_breakpad::PROCESS_OK) {
			return kPRLocalError;
		}

		std::string os_short = "";
		std::string cpu_arch = "";
		if (processState.system_info()) {
			os_short = processState.system_info()->os_short;
			if (os_short.empty()) {
				os_short = processState.system_info()->os;
			}
			cpu_arch = processState.system_info()->cpu;
		}

		int requestingThread = processState.requesting_thread();
		if (requestingThread == -1) {
			requestingThread = 0;
		}

		if (requestingThread < 0 || static_cast<size_t>(requestingThread) >= processState.threads()->size()) {
			return kPRLocalError;
		}
		const google_breakpad::CallStack *stack = processState.threads()->at(requestingThread);
		if (!stack) {
			return kPRLocalError;
		}

		int frameCount = stack->frames()->size();
		if (frameCount > 1024) {
			frameCount = 1024;
		}

		std::ostringstream summaryStream;
		summaryStream << 2 << "|" << processState.time_date_stamp() << "|" << os_short << "|" << cpu_arch << "|" << processState.crashed() << "|" << processState.crash_reason() << "|" << std::hex << processState.crash_address() << std::dec << "|" << requestingThread;

		std::map<const google_breakpad::CodeModule *, unsigned int> moduleMap;

		unsigned int moduleCount = processState.modules() ? processState.modules()->module_count() : 0;
		for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
			auto module = processState.modules()->GetModuleAtIndex(moduleIndex);
			moduleMap[module] = moduleIndex;

			auto debugFile = google_breakpad::PathnameStripper::File(module->debug_file());
			auto debugIdentifier = module->debug_identifier();

			summaryStream << "|M|" << debugFile << "|" << debugIdentifier;
		}

		for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
			auto frame = stack->frames()->at(frameIndex);

			int moduleIndex = -1;
			auto moduleOffset = frame->ReturnAddress();
			if (frame->module) {
				moduleIndex = moduleMap[frame->module];
				moduleOffset -= frame->module->base_address();
			}

			summaryStream << "|F|" << moduleIndex << "|" << std::hex << moduleOffset << std::dec;
		}

		auto summaryLine = summaryStream.str();
		// printf("%s\n", summaryLine.c_str());

		std::unique_ptr<IWebForm> form(webternet->CreateForm());
		if (!form) {
			return kPRLocalError;
		}

		const char *minidumpAccount = acceleratorConfig.GetValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		form->AddString("CrashSignature", summaryLine.c_str());

		MemoryDownloader data;
		std::unique_ptr<IWebTransfer> xfer(webternet->CreateSession());
		if (!xfer) {
			return kPRLocalError;
		}
		xfer->SetFailOnHTTPError(true);

		const char *minidumpUrl = acceleratorConfig.GetValue("MinidumpUrl");
		if (!minidumpUrl) minidumpUrl = "https://crash.limetech.org/submit";

		auto result = control.RunRequest([&]() {
			return xfer->PostAndDownload(minidumpUrl, form.get(), &data, NULL);
		});

		if (result == UploadRequestResult::NotStarted) {
			return kPRStopped;
		}
		if (result == UploadRequestResult::Failed) {
			if (log) fprintf(log, "Presubmit failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			return kPRRemoteError;
		}

		int responseSize = data.GetSize();
		char *response = data.GetBuffer();
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		//if (log) fprintf(log, "Presubmit complete: %s\n", response);

		if (responseSize < 2) {
			if (log) fprintf(log, "Presubmit response too short\n");
			return kPRRemoteError;
		}

		if (response[0] == 'E') {
			if (log) fprintf(log, "Presubmit error: %s\n", &response[2]);
			return kPRRemoteError;
		}

		PresubmitResponse presubmitResponse = kPRRemoteError;
		if (response[0] == 'Y') presubmitResponse = kPRUploadCrashDumpAndMetadata;
		else if (response[0] == 'N') presubmitResponse = kPRDontUpload;
		else if (response[0] == 'M') presubmitResponse = kPRUploadMetadataOnly;
		else return kPRRemoteError;

		if (response[1] != '|') {
			if (log) fprintf(log, "Response delimiter missing\n");
			return kPRRemoteError;
		}

		// A service-requested skip is complete; it needs no supporting uploads.
		if (presubmitResponse == kPRDontUpload) {
			return presubmitResponse;
		}

		unsigned int responseCount = responseSize - 2;
		if (responseCount < moduleCount) {
			if (log) fprintf(log, "Response module list doesn't match sent list (%d < %d)\n", responseCount, moduleCount);
			return kPRRemoteError;
		}

		// There was a presubmit token included.
		if (tokenBuffer && responseCount > moduleCount && response[2 + moduleCount] == '|') {
			int tokenStart = 2 + moduleCount + 1;
			int tokenEnd = tokenStart;
			while (tokenEnd < responseSize && response[tokenEnd] != '|') {
				tokenEnd++;
			}

			size_t tokenLength = tokenEnd - tokenStart;
			if (tokenLength < tokenBufferLength) {
				strncpy(tokenBuffer, &response[tokenStart], tokenLength);
				tokenBuffer[tokenLength] = '\0';
			}

			if (log) fprintf(log, "Got a presubmit token from server: %s\n", tokenBuffer);
		}

		if (moduleCount > 0) {
			auto mainModule = processState.modules()->GetMainModule();
			if (!mainModule) {
				return kPRLocalError;
			}
			auto executableBaseDir = PathnameStripper_Directory(mainModule->code_file());
			InitModuleClassificationMap(executableBaseDir);

			// 0 = Disabled
			// 1 = System Only
			// 2 = System + Game
			// 3 = System + Game + Addons
			const char *symbolSubmitOptionStr = acceleratorConfig.GetValue("MinidumpSymbolUpload");
			int symbolSubmitOption = symbolSubmitOptionStr ? atoi(symbolSubmitOptionStr) : 3;

			bool canBinarySubmit = acceleratorConfig.GetBool("MinidumpBinaryUpload", true);

			for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
				if (control.ShouldStop()) {
					return kPRStopped;
				}
				bool submitSymbols = false;
				bool submitBinary = (response[2 + moduleIndex] == 'U');

#if defined _LINUX
				submitSymbols = (response[2 + moduleIndex] == 'Y');
#endif

				if (!submitSymbols && !submitBinary) {
					continue;
				}
				if (log) fprintf(log, "Getting module at index %d\n", moduleIndex);
				if (log) fflush(log);

				auto module = processState.modules()->GetModuleAtIndex(moduleIndex);

				auto moduleType = ClassifyModule(module);
				if (log) fprintf(log, "Classified module %s as %s\n", module->code_file().c_str(), ModuleTypeCode[moduleType]);
				if (log) fflush(log);
				switch (moduleType) {
					case kMTUnknown:
						continue;
					case kMTSystem:
						if (symbolSubmitOption < 1) {
							continue;
						}
						break;
					case kMTGame:
						if (symbolSubmitOption < 2) {
							continue;
						}
						break;
					case kMTAddon:
					case kMTExtension:
						if (symbolSubmitOption < 3) {
							continue;
						}
						break;
				}

				if (canBinarySubmit && submitBinary) {
					UploadModuleFile(module, tokenBuffer);
					if (control.ShouldStop()) {
						return kPRStopped;
					}
				}

#if defined _LINUX
				if (submitSymbols) {
					UploadSymbolFile(module, tokenBuffer);
					if (control.ShouldStop()) {
						return kPRStopped;
					}
				}
#endif
			}
		}
		if (log) fprintf(log, "PresubmitCrashDump complete\n");
		if (log) fflush(log);

		return presubmitResponse;
	}

	bool UploadCrashDump(const char *path, const char *metapath, const char *presubmitToken, char *response, int maxlen) {
		std::unique_ptr<IWebForm> form(webternet->CreateForm());
		if (!form) {
			if (response && maxlen > 0) {
				g_pSM->Format(response, maxlen, "Failed to create upload form");
			}
			return false;
		}

		const char *minidumpAccount = acceleratorConfig.GetValue("MinidumpAccount");
		if (minidumpAccount && minidumpAccount[0]) form->AddString("UserID", minidumpAccount);

		form->AddString("GameDirectory", crashGameDirectory);
		form->AddString("ExtensionVersion", SMEXT_CONF_VERSION);
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		if (path && path[0]) {
			form->AddFile("upload_file_minidump", path);
		}

		if (metapath && metapath[0]) {
			form->AddFile("upload_file_metadata", metapath);
		}

		MemoryDownloader data;
		std::unique_ptr<IWebTransfer> xfer(webternet->CreateSession());
		if (!xfer) {
			if (response && maxlen > 0) {
				g_pSM->Format(response, maxlen, "Failed to create upload session");
			}
			return false;
		}
		xfer->SetFailOnHTTPError(true);

		const char *minidumpUrl = acceleratorConfig.GetValue("MinidumpUrl");
		if (!minidumpUrl) minidumpUrl = "https://crash.limetech.org/submit";

		auto result = control.RunRequest([&]() {
			return xfer->PostAndDownload(minidumpUrl, form.get(), &data, NULL);
		});
		bool uploaded = result == UploadRequestResult::Succeeded;

		if (response && maxlen > 0) {
			if (uploaded) {
				int responseSize = data.GetSize();
				if (responseSize >= maxlen) responseSize = maxlen - 1;
				strncpy(response, data.GetBuffer(), responseSize);
				response[responseSize] = '\0';
				while (responseSize > 0 && response[responseSize - 1] == '\n') {
					response[--responseSize] = '\0';
				}
			} else if (result == UploadRequestResult::NotStarted) {
				g_pSM->Format(response, maxlen, "Upload pass stopped before submission");
			} else {
				g_pSM->Format(response, maxlen, "%s (%d)", xfer->LastErrorMessage(), xfer->LastErrorCode());
			}
		}

		return uploaded;
	}
} uploadThread;

void Accelerator::NotifyIfDoneUploading()
{
	if (!IsMapStarted() || !IsDoneUploading() || !extforwards::IsInitialized()) {
		return;
	}

	if (!m_notified.exchange(true)) {
		extforwards::CallOnDoneUploadingForward();
	}
}

class VFuncEmptyClass {};

const char *GetCmdLine()
{
	if (!optionalGameHelpers) {
		return "";
	}
	static int getCmdLineOffset = 0;
	if (getCmdLineOffset == 0) {
		if (!gameconfig || !gameconfig->GetOffset("GetCmdLine", &getCmdLineOffset)) {
			return "";
		}
		if (getCmdLineOffset == 0) {
			return "";
		}
	}

	void *cmdline = optionalGameHelpers->GetValveCommandLine();
	if (!cmdline) {
		return "";
	}
	void **vtable = *(void ***)cmdline;
	if (!vtable) {
		return "";
	}
	void *vfunc = vtable[getCmdLineOffset];
	if (!vfunc) {
		return "";
	}

	union {
		const char *(VFuncEmptyClass::*mfpnew)();
#if !defined _WINDOWS
		struct {
			void *addr;
			intptr_t adjustor;
		} s;
	} u;
	u.s.addr = vfunc;
	u.s.adjustor = 0;
#else
		void *addr;
	} u;
	u.addr = vfunc;
#endif

	const char *line = (const char *)(reinterpret_cast<VFuncEmptyClass*>(cmdline)->*u.mfpnew)();
	return line ? line : "";
}

Accelerator::Accelerator() :
	m_doneuploading(false), m_maphasstarted(false), m_notified(false)
{
}

bool Accelerator::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	// Keep configuration immutable once the upload thread has started.
	char configPath[512];
	g_pSM->BuildPath(Path_SM, configPath, sizeof(configPath), "configs/accelerator.cfg");
	acceleratorConfig = AcceleratorConfig();
	if (libsys->PathExists(configPath)) {
		if (!acceleratorConfig.Load(textparsers, configPath, error, maxlength)) {
			return false;
		}
	} else {
		smutils->LogMessage(myself, "WARNING: %s not found; using Accelerator defaults. Settings in core.cfg are no longer read.", configPath);
	}

	// Metadata must not prevent the native crash handler from being installed.
	// Retry later if core interface registration is not yet complete.
	AcquireGameHelpers();

	if (!acceleratorConfig.GetBool("MinidumpDisableUpload", false)) {
		for (const char *key : {"MinidumpUrl", "MinidumpSymbolUrl", "MinidumpBinaryUrl"}) {
			const char *url = acceleratorConfig.GetValue(key);
			if (url && strncasecmp(url, "http://", 7) == 0) {
				smutils->LogMessage(myself, "WARNING: %s uses unencrypted HTTP. Update the endpoint to HTTPS to protect crash data.", key);
			}
		}
		sharesys->AddDependency(myself, "webternet.ext", true, true);
		SM_GET_IFACE(WEBTERNET, webternet);
	}

	g_pSM->BuildPath(Path_SM, dumpStoragePath, sizeof(dumpStoragePath), "data/dumps");

	if (!libsys->IsPathDirectory(dumpStoragePath))
	{
		if (!libsys->CreateFolder(dumpStoragePath))
		{
			if (error)
				g_pSM->Format(error, maxlength, "%s didn't exist and we couldn't create it :(", dumpStoragePath);
			return false;
		}
	}

	g_pSM->BuildPath(Path_SM, logPath, sizeof(logPath), "logs/accelerator.log");

	// Get these early so the upload thread can use them.
	strncpy(crashGamePath, g_pSM->GetGamePath(), sizeof(crashGamePath) - 1);
	strncpy(crashSourceModPath, g_pSM->GetSourceModPath(), sizeof(crashSourceModPath) - 1);
	strncpy(crashGameDirectory, g_pSM->GetGameFolderName(), sizeof(crashGameDirectory) - 1);

	do {
		char gameconfigError[256];
		if (!gameconfs->LoadGameConfigFile("accelerator.games", &gameconfig, gameconfigError, sizeof(gameconfigError))) {
			smutils->LogMessage(myself, "WARNING: Failed to load gamedata file, console output and command line will not be included in crash reports: %s", gameconfigError);
			break;
		}

		bool useFastcall = false;


#if defined _WINDOWS
		const char *fastcall = gameconfig->GetKeyValue("UseFastcall");
		if (fastcall && strcmp(fastcall, "yes") == 0) {
			useFastcall = true;
		}

		if (useFastcall && !gameconfig->GetMemSig("GetSpewFastcall", (void **)&GetSpewFastcall)) {
			smutils->LogMessage(myself, "WARNING: GetSpewFastcall not found in gamedata, console output will not be included in crash reports.");
			break;
		}
#endif

		if (!useFastcall && !gameconfig->GetMemSig("GetSpew", (void **)&GetSpew)) {
			smutils->LogMessage(myself, "WARNING: GetSpew not found in gamedata, console output will not be included in crash reports.");
			break;
		}

		if (!GetSpew
#if defined _WINDOWS
			&& !GetSpewFastcall
#endif
		) {
			smutils->LogMessage(myself, "WARNING: Sigscan for GetSpew failed, console output will not be included in crash reports.");
			break;
		}
	} while(false);

#if defined _LINUX
	google_breakpad::MinidumpDescriptor descriptor(dumpStoragePath);
	handler = new google_breakpad::ExceptionHandler(descriptor, NULL, dumpCallback, NULL, true, -1);

	struct sigaction oact;
	sigaction(SIGSEGV, NULL, &oact);
	SignalHandler = oact.sa_sigaction;

	previousTerminateHandler = std::set_terminate(terminateHandler);
#elif defined _WINDOWS
	wchar_t buf[sizeof(dumpStoragePath)];
	size_t num_chars = mbstowcs(buf, dumpStoragePath, sizeof(dumpStoragePath));
	if (num_chars == static_cast<size_t>(-1) || num_chars >= sizeof(dumpStoragePath)) {
		if (error && maxlength) {
			g_pSM->Format(error, maxlength, "Cannot convert Accelerator dump directory to a Windows path");
		}
		return false;
	}

	handler = new google_breakpad::ExceptionHandler(
		std::wstring(buf, num_chars), NULL, dumpCallback, NULL, google_breakpad::ExceptionHandler::HANDLER_ALL,
		static_cast<MINIDUMP_TYPE>(MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo), static_cast<const wchar_t *>(NULL), NULL);

	windowsFirstChance = acceleratorConfig.GetBool("MinidumpWindowsFirstChance", true);
	windowsCaptureGate.Reset();
	// First in the vectored chain at registration time, before existing engine
	// handlers. Later third-party registrations can still change that ordering.
	vectoredHandler = AddVectoredExceptionHandler(1, BreakpadVectoredHandler);
	if (!vectoredHandler) {
		smutils->LogError(myself, "Failed to register Accelerator's Windows vectored handler (error %lu); engine-intercepted crashes may not be captured.", GetLastError());
	} else {
		rootconsole->ConsolePrint("Accelerator Windows first-chance crash capture: %s", windowsFirstChance ? "enabled" : "disabled (fatal Sys_Error only)");
	}

#else
#error Bad platform.
#endif

	g_pSM->AddGameFrameHook(OnGameFrame);

#if SMINTERFACE_EXTENSIONAPI_VERSION < 9
	// 1.13 links the SourcePawn VM into sourcemod.logic and dropped ISourcePawnFactory,
	// so there is no sourcepawn.jit library left to load here.
	do {
		char spJitPath[512];
		g_pSM->BuildPath(Path_SM, spJitPath, sizeof(spJitPath), "bin/" PLATFORM_ARCH_FOLDER "sourcepawn.jit.x86." PLATFORM_LIB_EXT);

		char spJitError[255];
		std::unique_ptr<ILibrary> spJit(libsys->OpenLibrary(spJitPath, spJitError, sizeof(spJitError)));
		if (!spJit) {
			smutils->LogMessage(myself, "WARNING: Failed to load SourcePawn library %s: %s", spJitPath, spJitError);
			break;
		}

		GetSourcePawnFactoryFn factoryFn = (GetSourcePawnFactoryFn)spJit->GetSymbolAddress("GetSourcePawnFactory");
		if (!factoryFn) {
			smutils->LogMessage(myself, "WARNING: SourcePawn library is out of date: No factory function.");
			break;
		}

		ISourcePawnFactory *spFactory = factoryFn(0x0207);
		if (!spFactory) {
			smutils->LogMessage(myself, "WARNING: SourcePawn library is out of date: Failed to get version 2.7");
			break;
		}

		ISourcePawnEnvironment *spEnvironment = spFactory->CurrentEnvironment();
		if (!spEnvironment) {
			smutils->LogMessage(myself, "WARNING: Could not get SourcePawn environment.");
			break;
		}

		ISourcePawnEngine2 *spEngine2 = spEnvironment->APIv2();
		if (!spEngine2) {
			smutils->LogMessage(myself, "WARNING: Could not get SourcePawn engine2.");
			break;
		}

		strncpy(crashSourceModVersion, spEngine2->GetVersionString(), sizeof(crashSourceModVersion) - 1);
	} while(false);
#endif

	plsys->AddPluginsListener(this);

	IPluginIterator *iterator = plsys->GetPluginIterator();
	while (iterator->MorePlugins()) {
		IPlugin *plugin = iterator->GetPlugin();
		if (plugin->GetStatus() == Plugin_Running) {
			this->OnPluginLoaded(plugin);
		}
		iterator->NextPlugin();
	}
	delete iterator;

	strncpy(crashCommandLine, GetCmdLine(), sizeof(crashCommandLine) - 1);

	char steamInfPath[512];
	g_pSM->BuildPath(Path_Game, steamInfPath, sizeof(steamInfPath), "steam.inf");

	FILE *steamInfFile = fopen(steamInfPath, "rb");
	if (steamInfFile) {
		char steamInfTemp[1024] = {0};
		fread(steamInfTemp, sizeof(char), sizeof(steamInfTemp) - 1, steamInfFile);

		fclose(steamInfFile);

		BuildSteamInf(steamInfTemp, steamInf, sizeof(steamInf));
	}

	// Start only after the handler and all crash metadata are initialized.
	if (acceleratorConfig.GetBool("MinidumpDisableUpload", false)) {
		rootconsole->ConsolePrint("Accelerator uploads are disabled; crash dumps will be kept locally.");
		MarkAsDoneUploading();
	} else {
		uploadThread.Reset();
		m_uploadThread = threader->MakeThread(&uploadThread, Thread_Default);
		if (!m_uploadThread) {
			smutils->LogError(myself, "Failed to start Accelerator upload thread; dumps remain on disk.");
			MarkAsDoneUploading();
		}
	}

	if (late) {
		this->OnCoreMapStart(NULL, 0, 0);
	}

	return true;
}

void Accelerator::SDK_OnUnload()
{
	uploadThread.RequestStop();
	g_pSM->RemoveGameFrameHook(OnGameFrame);
	// Do not release interfaces, forwards or extension code while the worker
	// might still be using them. WaitForThread also waits for OnTerminate.
	if (m_uploadThread) {
		m_uploadThread->WaitForThread();
		m_uploadThread->DestroyThis();
		m_uploadThread = nullptr;
	}
	extforwards::Shutdown();
	plsys->RemovePluginsListener(this);

#if defined _LINUX
	if (std::get_terminate() == terminateHandler) {
		std::set_terminate(previousTerminateHandler);
	}
#elif defined _WINDOWS
	windowsCaptureGate.RequestStop();
	if (vectoredHandler) {
		RemoveVectoredExceptionHandler(vectoredHandler);
		vectoredHandler = nullptr;
	}
	// Do not destroy Breakpad while a vectored callback is using it.
	while (windowsCaptureGate.IsBusy()) {
		Sleep(1);
	}
#else
#error Bad platform.
#endif

	delete handler;
	handler = nullptr;
	optionalGameHelpers = nullptr;
}

void Accelerator::SDK_OnAllLoaded()
{
	if (!AcquireGameHelpers()) {
		smutils->LogMessage(myself,
			"WARNING: IGameHelpers interface version %u unavailable for Accelerator %s; crash capture remains active, but map and command-line metadata may be absent.",
			static_cast<unsigned int>(SMINTERFACE_GAMEHELPERS_VERSION), SMEXT_CONF_VERSION);
	}
	strncpy(crashCommandLine, GetCmdLine(), sizeof(crashCommandLine) - 1);
	extforwards::Init();
	sharesys->RegisterLibrary(myself, "accelerator");

	natives::Setup(m_natives);
	m_natives.push_back({ nullptr, nullptr }); // SM requires this to signal the end of the native info array

	sharesys->AddNatives(myself, m_natives.data());
}

void Accelerator::OnCoreMapStart(edict_t *pEdictList, int edictCount, int clientMax)
{
	crashMap[0] = '\0';
	if (AcquireGameHelpers()) {
		const char *map = optionalGameHelpers->GetCurrentMap();
		if (map) {
			strncpy(crashMap, map, sizeof(crashMap) - 1);
		}
	}
	strncpy(crashCommandLine, GetCmdLine(), sizeof(crashCommandLine) - 1);
	m_maphasstarted.store(true);
}

/* 010 Editor Template
uint64 headerMagic;
uint32 version;
uint32 size;
uint32 count;
struct {
    uint32 size;
    uint32 context <format=hex>;
    char file[];
    uint32 count;
    struct {
        uint32 pcode <format=hex>;
        char name[];
    } functions[count] <optimize=false>;
} plugins[count] <optimize=false>;
uint64 tailMagic;
*/

unsigned char *serializedPluginContexts = nullptr;
std::map<const IPluginContext *, unsigned char *> pluginContextMap;

void SerializePluginContexts()
{
	if (serializedPluginContexts) {
		handler->UnregisterAppMemory(serializedPluginContexts);
		free(serializedPluginContexts);
		serializedPluginContexts = nullptr;
	}

	uint32_t count = pluginContextMap.size();
	if (count == 0) {
		return;
	}

	uint32_t size = 0;
	size += sizeof(uint64_t); // header magic
	size += sizeof(uint32_t); // version
	size += sizeof(uint32_t); // size
	size += sizeof(uint32_t); // count

	for (auto &it : pluginContextMap) {
		unsigned char *buffer = it.second;

		uint32_t bufferSize;
		memcpy(&bufferSize, buffer, sizeof(uint32_t));

		size += bufferSize;
	}

	size += sizeof(uint64_t); // tail magic

	serializedPluginContexts = (unsigned char *)malloc(size);
	handler->RegisterAppMemory(serializedPluginContexts, size);
	unsigned char *cursor = serializedPluginContexts;

	uint64_t headerMagic = 103582791429521979ULL;
	memcpy(cursor, &headerMagic, sizeof(uint64_t));
	cursor += sizeof(uint64_t);

	uint32_t version = 1;
	memcpy(cursor, &version, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	memcpy(cursor, &size, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	memcpy(cursor, &count, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	for (auto &it : pluginContextMap) {
		unsigned char *buffer = it.second;

		uint32_t bufferSize;
		memcpy(&bufferSize, buffer, sizeof(uint32_t));

		memcpy(cursor, buffer, bufferSize);
		cursor += bufferSize;
	}

	uint64_t tailMagic = 76561197987819599ULL;
	memcpy(cursor, &tailMagic, sizeof(uint64_t));
	cursor += sizeof(uint64_t);
}

void Accelerator::OnPluginLoaded(IPlugin *plugin)
{
	IPluginRuntime *runtime = plugin->GetRuntime();
	IPluginContext *context = plugin->GetBaseContext();
	if (!runtime || !context) {
		return;
	}

	AcceleratorPublicRuntime *publics = GetAcceleratorPublicRuntime(runtime);
	if (!publics) {
		return;
	}

	const char *filename = plugin->GetFilename();
	size_t filenameSize = strlen(filename) + 1;

	uint32_t size = 0;
	size += sizeof(uint32_t); // size
	size += sizeof(void *); // GetBaseContext
	size += filenameSize;

	uint32_t count = publics->GetPublicsNum();
	size += sizeof(uint32_t); // count
	size += count * sizeof(uint32_t); // pubinfo->code_offs

	for (uint32_t i = 0; i < count; ++i) {
		sp_public_t *pubinfo;
		publics->GetPublicByIndex(i, &pubinfo);

		size += strlen(pubinfo->name) + 1;
	}

	unsigned char *buffer = (unsigned char *)malloc(size);
	unsigned char *cursor = buffer;

	memcpy(cursor, &size, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	memcpy(cursor, &context, sizeof(void *));
	cursor += sizeof(void *);

	memcpy(cursor, filename, filenameSize);
	cursor += filenameSize;

	memcpy(cursor, &count, sizeof(uint32_t));
	cursor += sizeof(uint32_t);

	for (uint32_t i = 0; i < count; ++i) {
		sp_public_t *pubinfo;
		publics->GetPublicByIndex(i, &pubinfo);

		memcpy(cursor, &pubinfo->code_offs, sizeof(uint32_t));
		cursor += sizeof(uint32_t);

		size_t nameSize = strlen(pubinfo->name) + 1;
		memcpy(cursor, pubinfo->name, nameSize);
		cursor += nameSize;
	}

	pluginContextMap[context] = buffer;

	SerializePluginContexts();
}

void Accelerator::OnPluginUnloaded(IPlugin *plugin)
{
	IPluginContext *context = plugin->GetBaseContext();
	if (!context) {
		return;
	}

	auto it = pluginContextMap.find(context);
	if (it != pluginContextMap.end()) {
		free(it->second);
		pluginContextMap.erase(it);
	}

	SerializePluginContexts();
}

void Accelerator::StoreUploadedCrash(UploadedCrash& crash)
{
	std::lock_guard<std::mutex> lock(m_uploadedcrashes_mutex);
	m_uploadedcrashes.push_back(std::move(crash));
}

const UploadedCrash* Accelerator::GetUploadedCrash(int element) const
{
	std::lock_guard<std::mutex> lock(m_uploadedcrashes_mutex);
	if (element < 0 || element >= static_cast<int>(m_uploadedcrashes.size())) {
		return nullptr;
	}

	return &m_uploadedcrashes[element];
}

cell_t Accelerator::GetUploadedCrashCount() const
{
	std::lock_guard<std::mutex> lock(m_uploadedcrashes_mutex);
	return static_cast<cell_t>(m_uploadedcrashes.size());
}
