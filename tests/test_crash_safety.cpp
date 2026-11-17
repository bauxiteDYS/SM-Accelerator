#include "CrashUtils.h"
#include "MemoryDownloader.h"
#include "AcceleratorConfig.h"
#include "DumpUploadQueue.h"
#include "UploadPassControl.h"
#include "WindowsExceptionPolicy.h"
#include <cassert>
#include <limits>
#include <map>
#include <vector>

using namespace SourceMod;

class Parser : public ITextParsers {
public:
	std::vector<std::pair<const char *, const char *>> values;
	SMCError ParseFile_SMC(const char *, ITextListener_SMC *listener, SMCStates *states) override {
		listener->ReadSMC_ParseStart();
		if (listener->ReadSMC_NewSection(states, "Accelerator") != SMCResult_Continue)
			return SMCError_Custom;
		for (auto &value : values) {
			if (listener->ReadSMC_KeyValue(states, value.first, value.second) != SMCResult_Continue)
				return SMCError_Custom;
		}
		listener->ReadSMC_LeavingSection(states);
		return SMCError_Okay;
	}
	const char *GetSMCErrorString(SMCError) override { return "parse error"; }
};

int main()
{
	assert(ShouldCaptureWindowsException(0xC0000005u, 0, true)); // access violation
	assert(ShouldCaptureWindowsException(0xC00000FDu, 0, true)); // stack overflow
	assert(!ShouldCaptureWindowsException(0xC0000005u, 0, false));
	assert(!ShouldCaptureWindowsException(0x80000003u, 0, true)); // breakpoint
	assert(!ShouldCaptureWindowsException(0x80000004u, 0, true)); // single step
	assert(!ShouldCaptureWindowsException(0xE06D7363u, 0, true)); // C++ throw
	assert(!ShouldCaptureWindowsException(0, 0, true)); // continuable code zero
	assert(ShouldCaptureWindowsException(0, 1, true));
	assert(ShouldCaptureWindowsException(0, 1, false)); // fatal Sys_Error retained
	WindowsCaptureGate gate;
	assert(gate.TryBegin());
	assert(gate.IsBusy());
	assert(!gate.TryBegin()); // no nested/concurrent explicit writer
	gate.RequestStop();
	assert(gate.IsBusy()); // unload must wait for this callback
	gate.Finish();
	assert(!gate.IsBusy());
	assert(!gate.TryBegin()); // cannot acquire during unload
	gate.Reset();
	assert(gate.TryBegin());
	gate.Finish();

	UploadPassControl pass;
	int requests = 0;
	assert(pass.RunRequest([&]() { ++requests; return false; }) == UploadRequestResult::Failed);
	assert(pass.HasFailed() && pass.ShouldStop());
	assert(pass.RunRequest([&]() { ++requests; return true; }) == UploadRequestResult::NotStarted);
	assert(requests == 1); // no fallback, supporting request, or second dump
	pass.Reset();
	assert(!pass.HasFailed() && !pass.ShouldStop());
	pass.RequestStop(); // unload before the worker starts
	assert(pass.RunRequest([&]() { ++requests; return true; }) == UploadRequestResult::NotStarted);
	assert(requests == 1 && !pass.HasFailed());
	pass.Reset();
	// A final upload accepted during unload is still successful. Its completion
	// must be recorded, while the next request is suppressed.
	assert(pass.RunRequest([&]() {
		++requests;
		pass.RequestStop();
		return true;
	}) == UploadRequestResult::Succeeded);
	assert(pass.ShouldStop() && !pass.HasFailed());
	assert(pass.RunRequest([&]() { ++requests; return true; }) == UploadRequestResult::NotStarted);
	assert(requests == 2);
	pass.Reset();
	assert(pass.RunRequest([&]() { ++requests; return true; }) == UploadRequestResult::Succeeded);
	assert(pass.RunRequest([&]() { ++requests; return false; }) == UploadRequestResult::Failed);
	assert(pass.RunRequest([&]() { ++requests; return true; }) == UploadRequestResult::NotStarted);
	assert(requests == 4);
	pass.Reset();
	pass.RecordFailure(); // malformed/error presubmit response
	assert(pass.RunRequest([&]() { ++requests; return true; }) == UploadRequestResult::NotStarted);
	assert(requests == 4);

	DumpUploadQueue newest(2);
	newest.Consider("old.dmp", 10);
	newest.Consider("newest.dmp", 40);
	newest.Consider("middle.dmp", 20);
	newest.Consider("second.dmp", 30);
	assert(newest.Entries().size() == 2);
	auto selected = newest.Entries().begin();
	assert(selected++->path == "newest.dmp");
	assert(selected->path == "second.dmp");
	DumpUploadQueue ties(2);
	ties.Consider("c.dmp", 50);
	ties.Consider("a.dmp", 50);
	ties.Consider("b.dmp", 50);
	assert(ties.Entries().begin()->path == "a.dmp");
	assert(ties.Entries().rbegin()->path == "b.dmp");
	DumpUploadQueue backlog(2);
	for (int i = 0; i < 10000; ++i) {
		backlog.Consider(std::to_string(i) + ".dmp", i);
		assert(backlog.Entries().size() <= 2);
	}
	assert(backlog.Entries().begin()->modified == 9999);
	assert(backlog.Entries().rbegin()->modified == 9998);
	DumpUploadQueue single(2);
	assert(single.Entries().empty());
	single.Consider("only.dmp", 1);
	single.Consider("only.dmp", 1);
	assert(single.Entries().size() == 1);
	DumpUploadQueue none(0);
	none.Consider("never.dmp", 100);
	assert(none.Entries().empty());

	PathComparator less;
	assert(!less("/same", "/same"));
	assert(less("/game/addons/", "/game/"));
	assert(!less("/game/", "/game/addons/"));
	std::map<std::string, int, PathComparator> paths;
	paths["/game/"] = 1;
	paths["/game/addons/"] = 2;
	paths["/game/addons/"] = 3;
	assert(paths.size() == 2 && paths.begin()->second == 3);
#if defined _WINDOWS
	assert(!less("C:\\GAME\\", "c:\\game\\"));
	assert(!less("c:\\game\\", "C:\\GAME\\"));
#endif

	struct { char before; char output[1024]; char after; } guarded = {'a', {}, 'z'};
	std::string input;
	for (int i = 0; i < 511; ++i) input += "a\n";
	input += "a";
	BuildSteamInf(input.c_str(), guarded.output, sizeof(guarded.output));
	assert(guarded.before == 'a' && guarded.after == 'z');
	assert(std::strlen(guarded.output) <= sizeof(guarded.output) - 1);
	assert(std::strlen(guarded.output) % 8 == 0); // complete "\nSteam_a" records
	BuildSteamInf("PatchVersion=1\r\n//comment\nProductName=test\n", guarded.output, sizeof(guarded.output));
	assert(std::string(guarded.output) == "\nSteam_PatchVersion=1\nSteam_ProductName=test");
	char tiny[1] = {'x'};
	BuildSteamInf("key=value", tiny, sizeof(tiny));
	assert(tiny[0] == '\0');
	BuildSteamInf("", nullptr, 0);

	MemoryDownloader download;
	assert(download.GetSize() == 0 && download.GetBuffer()[0] == '\0');
	char bytes[] = {'a', 'b', 'c'}; // intentionally not null-terminated
	assert(download.OnDownloadWrite(nullptr, nullptr, bytes, 1, 3) == DownloadWrite_Okay);
	assert(download.GetSize() == 3 && std::strcmp(download.GetBuffer(), "abc") == 0);
	assert(download.OnDownloadWrite(nullptr, nullptr, nullptr, 0, 123) == DownloadWrite_Okay);
	assert(download.OnDownloadWrite(nullptr, nullptr, bytes, 2, std::numeric_limits<size_t>::max()) == DownloadWrite_Error);
	assert(download.GetSize() == 3);
	download.Reset();
	assert(download.GetSize() == 0 && download.GetBuffer()[0] == '\0');
	std::vector<char> maximum(1024 * 1024, 'a');
	assert(download.OnDownloadWrite(nullptr, nullptr, maximum.data(), 1, maximum.size()) == DownloadWrite_Okay);
	assert(download.GetBuffer()[maximum.size()] == '\0');
	assert(download.OnDownloadWrite(nullptr, nullptr, bytes, 1, 1) == DownloadWrite_Error);
	assert(download.GetSize() == maximum.size());

	AcceleratorConfig config;
	Parser parser;
	char error[512];
	assert(config.Load(&parser, "test", error, sizeof(error)));
	assert(config.GetBool("MinidumpKeepDumps", true));
	assert(!config.GetBool("MinidumpDisableUpload", false));
	assert(config.GetBool("MinidumpWindowsFirstChance", true));
	parser.values = {{"MinidumpWindowsFirstChance", "no"}};
	assert(config.Load(&parser, "test", error, sizeof(error)));
	assert(!config.GetBool("MinidumpWindowsFirstChance", true));
	parser.values = {{"MinidumpWindowsFirstChance", "maybe"}};
	assert(!config.Load(&parser, "test", error, sizeof(error)));
	parser.values.clear();
	assert(config.Load(&parser, "test", error, sizeof(error)));
	assert(config.GetValue("MinidumpUploadLimit") == nullptr); // caller defaults to 2
	parser.values = {{"MinidumpUploadLimit", "2"}};
	assert(config.Load(&parser, "test", error, sizeof(error)));
	assert(std::strcmp(config.GetValue("MinidumpUploadLimit"), "2") == 0);
	parser.values = {{"MinidumpUploadLimit", "1000"}};
	assert(config.Load(&parser, "test", error, sizeof(error)));
	for (const char *invalid : {"", "0", "-1", "+2", "1001", "2.5", "two", " 2", "99999999999999999999"}) {
		parser.values = {{"MinidumpUploadLimit", invalid}};
		assert(!config.Load(&parser, "test", error, sizeof(error)));
		assert(config.GetValue("MinidumpUploadLimit") == nullptr);
	}
	parser.values = {{"MinidumpDisableUpload", "TRUE"}, {"MinidumpKeepDumps", "False"}};
	assert(config.Load(&parser, "test", error, sizeof(error)));
	assert(config.GetBool("MinidumpDisableUpload", false));
	assert(!config.GetBool("MinidumpKeepDumps", true));
	parser.values = {{"MinidumpDisableUpload", "maybe"}};
	assert(!config.Load(&parser, "test", error, sizeof(error)));
	assert(config.GetValue("MinidumpDisableUpload") == nullptr);
	parser.values = {{"MinidumpDisableUploads", "yes"}};
	assert(!config.Load(&parser, "test", error, sizeof(error)));
	parser.values = {{"MinidumpSymbolUpload", "4"}};
	assert(!config.Load(&parser, "test", error, sizeof(error)));
}
