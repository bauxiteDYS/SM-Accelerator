#ifndef _INCLUDE_ACCELERATOR_CRASH_UTILS_H_
#define _INCLUDE_ACCELERATOR_CRASH_UTILS_H_

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>

// Descending order puts a more specific directory before its parent.
struct PathComparator {
	struct CompareChar {
		bool operator()(unsigned char a, unsigned char b) const {
#if defined _WINDOWS
			return std::tolower(a) < std::tolower(b);
#else
			return a < b;
#endif
		}
	};
	bool operator()(const std::string &a, const std::string &b) const {
		return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end(), CompareChar());
	}
};

// Format whole steam.inf lines only; never truncate a key/value or the terminator.
inline void BuildSteamInf(const char *input, char *output, size_t capacity)
{
	if (!capacity) {
		return;
	}
	size_t used = 0;
	output[0] = '\0';
	while (*input) {
		const char *start = input;
		while (*input && *input != '\n' && !(input[0] == '/' && input[1] == '/')) {
			++input;
		}
		const char *end = input;
		if (end > start && end[-1] == '\r') {
			--end;
		}
		size_t length = end - start;
		if (length) {
			const char prefix[] = "\nSteam_";
			if (sizeof(prefix) - 1 + length >= capacity - used) {
				break;
			}
			std::memcpy(output + used, prefix, sizeof(prefix) - 1);
			used += sizeof(prefix) - 1;
			std::memcpy(output + used, start, length);
			used += length;
			output[used] = '\0';
		}
		while (*input && *input != '\n') {
			++input;
		}
		if (*input) {
			++input;
		}
	}
}

#endif
