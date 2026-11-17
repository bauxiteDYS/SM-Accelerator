// Minimal test double for SourceMod's SMC listener/parser interface.
#pragma once
#include <cstddef>
namespace SourceMod {
enum SMCResult { SMCResult_Continue, SMCResult_Halt, SMCResult_HaltFail };
enum SMCError { SMCError_Okay, SMCError_StreamOpen, SMCError_Custom };
struct SMCStates { unsigned int line, col; };
class ITextListener_SMC {
public:
	virtual void ReadSMC_ParseStart() {}
	virtual SMCResult ReadSMC_NewSection(const SMCStates *, const char *) { return SMCResult_Continue; }
	virtual SMCResult ReadSMC_KeyValue(const SMCStates *, const char *, const char *) { return SMCResult_Continue; }
	virtual SMCResult ReadSMC_LeavingSection(const SMCStates *) { return SMCResult_Continue; }
};
class ITextParsers {
public:
	virtual SMCError ParseFile_SMC(const char *, ITextListener_SMC *, SMCStates *) = 0;
	virtual const char *GetSMCErrorString(SMCError) = 0;
};
}
