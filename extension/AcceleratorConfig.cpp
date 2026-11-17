#include "AcceleratorConfig.h"

#include <cstdio>
#include <cstring>
#include <cctype>

using namespace SourceMod;

bool AcceleratorConfig::Load(ITextParsers *parser, const char *path, char *error, size_t maxlen)
{
	ReadSMC_ParseStart();
	SMCStates states = {};
	SMCError result = parser->ParseFile_SMC(path, this, &states);
	if (result == SMCError_Okay && m_seenRoot && !m_inSection) {
		return true;
	}

	const char *message = m_parseError;
	if (!message && result != SMCError_Okay) {
		message = parser->GetSMCErrorString(result);
	}
	if (!message) {
		message = "Expected a single \"Accelerator\" section";
	}
	if (error && maxlen) {
		std::snprintf(error, maxlen, "%s (line %u, column %u): %s",
			path, states.line, states.col, message);
	}

	// Never expose a partially parsed configuration.
	m_values.clear();
	return false;
}

const char *AcceleratorConfig::GetValue(const char *key) const
{
	auto entry = m_values.find(key);
	return entry == m_values.end() ? nullptr : entry->second.c_str();
}

bool AcceleratorConfig::GetBool(const char *key, bool defaultValue) const
{
	const char *value = GetValue(key);
	return value ? (value[0] == 'y' || value[0] == 'Y' || value[0] == '1') : defaultValue;
}

void AcceleratorConfig::ReadSMC_ParseStart()
{
	m_values.clear();
	m_seenRoot = false;
	m_inSection = false;
	m_parseError = nullptr;
}

SMCResult AcceleratorConfig::ReadSMC_NewSection(const SMCStates *states, const char *name)
{
	if (m_seenRoot || std::strcmp(name, "Accelerator") != 0) {
		m_parseError = "Expected a single \"Accelerator\" section with no nested sections";
		return SMCResult_HaltFail;
	}
	m_seenRoot = true;
	m_inSection = true;
	return SMCResult_Continue;
}

SMCResult AcceleratorConfig::ReadSMC_KeyValue(const SMCStates *states, const char *key, const char *value)
{
	if (!m_inSection || !value) {
		m_parseError = "Expected a key/value pair inside the \"Accelerator\" section";
		return SMCResult_HaltFail;
	}
	const char *boolKeys[] = {
		"MinidumpKeepDumps", "MinidumpDisableUpload", "MinidumpPresubmit", "MinidumpBinaryUpload",
		"MinidumpWindowsFirstChance"
	};
	for (const char *boolKey : boolKeys) {
		if (std::strcmp(key, boolKey) == 0) {
			std::string option(value);
			for (char &c : option) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (option == "yes" || option == "true" || option == "1") {
				m_values[key] = "yes";
			} else if (option == "no" || option == "false" || option == "0") {
				m_values[key] = "no";
			} else {
				m_parseError = "Boolean settings require yes/no, true/false, or 1/0";
				return SMCResult_HaltFail;
			}
			return SMCResult_Continue;
		}
	}
	if (std::strcmp(key, "MinidumpUploadLimit") == 0) {
		unsigned int limit = 0;
		bool valid = value[0] != '\0';
		for (const char *digit = value; valid && *digit; ++digit) {
			if (*digit < '0' || *digit > '9') {
				valid = false;
				break;
			}
			limit = limit * 10 + (*digit - '0');
			valid = limit <= 1000;
		}
		if (!valid || limit == 0) {
			m_parseError = "MinidumpUploadLimit must be an integer between 1 and 1000";
			return SMCResult_HaltFail;
		}
	} else if (std::strcmp(key, "MinidumpSymbolUpload") == 0) {
		if (std::strlen(value) != 1 || value[0] < '0' || value[0] > '3') {
			m_parseError = "MinidumpSymbolUpload must be 0, 1, 2, or 3";
			return SMCResult_HaltFail;
		}
	} else if (std::strcmp(key, "MinidumpAccount") != 0
		&& std::strcmp(key, "MinidumpUrl") != 0
		&& std::strcmp(key, "MinidumpSymbolUrl") != 0
		&& std::strcmp(key, "MinidumpBinaryUrl") != 0) {
		m_parseError = "Unknown Accelerator setting (setting names are case-sensitive)";
		return SMCResult_HaltFail;
	}
	m_values[key] = value;
	return SMCResult_Continue;
}

SMCResult AcceleratorConfig::ReadSMC_LeavingSection(const SMCStates *states)
{
	m_inSection = false;
	return SMCResult_Continue;
}
