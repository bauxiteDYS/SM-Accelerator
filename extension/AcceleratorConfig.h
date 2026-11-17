#ifndef _INCLUDE_ACCELERATOR_CONFIG_H_
#define _INCLUDE_ACCELERATOR_CONFIG_H_

#include <cstddef>
#include <ITextParsers.h>
#include <map>
#include <string>

// Loaded once before the upload thread starts, then only read by that thread.
class AcceleratorConfig : public SourceMod::ITextListener_SMC
{
public:
	bool Load(SourceMod::ITextParsers *parser, const char *path, char *error, size_t maxlen);
	const char *GetValue(const char *key) const;
	bool GetBool(const char *key, bool defaultValue) const;

private:
	void ReadSMC_ParseStart() override;
	SourceMod::SMCResult ReadSMC_NewSection(const SourceMod::SMCStates *states, const char *name) override;
	SourceMod::SMCResult ReadSMC_KeyValue(const SourceMod::SMCStates *states, const char *key, const char *value) override;
	SourceMod::SMCResult ReadSMC_LeavingSection(const SourceMod::SMCStates *states) override;

	std::map<std::string, std::string> m_values;
	bool m_seenRoot = false;
	bool m_inSection = false;
	const char *m_parseError = nullptr;
};

#endif // _INCLUDE_ACCELERATOR_CONFIG_H_
