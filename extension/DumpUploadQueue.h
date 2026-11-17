#ifndef _INCLUDE_ACCELERATOR_DUMP_UPLOAD_QUEUE_H_
#define _INCLUDE_ACCELERATOR_DUMP_UPLOAD_QUEUE_H_

#include <cstddef>
#include <ctime>
#include <set>
#include <string>

struct DumpCandidate {
	std::string path;
	time_t modified;
};

struct NewestDumpFirst {
	bool operator()(const DumpCandidate &a, const DumpCandidate &b) const {
		if (a.modified != b.modified) {
			return a.modified > b.modified;
		}
		// File timestamps may have only second-level precision.
		return a.path < b.path;
	}
};

// Scan the whole directory, but retain only the newest limit candidates.
// Memory is O(limit), and selection is O(number of dumps * log(limit)).
class DumpUploadQueue {
public:
	explicit DumpUploadQueue(size_t limit) : m_limit(limit) {}

	void Consider(const std::string &path, time_t modified) {
		m_entries.insert({path, modified});
		if (m_entries.size() > m_limit) {
			auto oldest = m_entries.end();
			m_entries.erase(--oldest);
		}
	}

	const std::set<DumpCandidate, NewestDumpFirst> &Entries() const {
		return m_entries;
	}

private:
	size_t m_limit;
	std::set<DumpCandidate, NewestDumpFirst> m_entries;
};

#endif
