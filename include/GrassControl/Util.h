#pragma once

namespace Util
{
	inline std::string _ovFilePath;
	const std::string _progressFilePath = R"(PrecacheGrass.txt)";

	std::string getProgressFilePath();

	void nopBlock(uintptr_t addr, int size, int offset);

	class CachedFormList final
	{
	public:
		CachedFormList();

		static std::unique_ptr<CachedFormList> TryParse(const std::string& input, std::string settingNameForLog, bool warnOnMissingForm = true, bool dontWriteAnythingToLog = false);

		void printList(std::string settingNameForLog) const;

		bool Contains(RE::TESForm* form) const;

		bool Contains(unsigned int formId) const;

		std::unordered_set<RE::TESForm*> getAll() const;

	private:
		std::unordered_set<RE::TESForm*> Forms = std::unordered_set<RE::TESForm*>();

		std::unordered_set<RE::FormID> Ids = std::unordered_set<RE::FormID>();
	};

	void report_and_fail_timed(const std::string& a_message);

	// Function object for case insensitive comparison
	struct case_insensitive_compare
	{
		case_insensitive_compare() {}

		// Function objects overloader operator()
		// When used as a comparer, it should function as operator<(a,b)
		bool operator()(const std::wstring& a, const std::wstring& b) const
		{
			return to_lower(a) < to_lower(b);
		}

		static std::wstring to_lower(const std::wstring& a)
		{
			std::wstring s(a);
			for (auto& c : s) {
				c = static_cast<wchar_t>(tolower(c));
			}
			//	transform(a.begin(), a.end(), a.begin(), ::tolower);
			return s;
		}

		static void char_to_lower(char& c)
		{
			if (c >= 'A' && c <= 'Z')
				c += ('a' - 'A');
		}
	};
	struct StringHelpers
	{
		template <typename Range, typename Value = typename Range::value_type>
		static std::string Join(Range const& elements, const char* const delimiter)
		{
			std::ostringstream os;
			auto b = begin(elements), e = end(elements);

			if (b != e) {
				std::copy(b, prev(e), std::ostream_iterator<Value>(os, delimiter));
				b = prev(e);
			}
			if (b != e) {
				os << *b;
			}

			return os.str();
		}

		static std::vector<std::string> split(const std::string& str, char delim, const bool RemoveEmpty)
		{
			std::stringstream ss(str);
			std::string token;
			std::vector<std::string> tokens;
			while (getline(ss, token, delim)) {
				tokens.push_back(token);
			}
			if (RemoveEmpty) {
				for (int i = 0; i < tokens.size();) {
					if (tokens[i].empty()) {
						tokens.erase(tokens.begin() + i);
					} else
						++i;
				}
			}
			return tokens;
		}

		static std::vector<std::string> Split_at_any(const std::string& str, const std::vector<char>& delims, const bool RemoveEmpty)
		{
			// Make a copy of the string
			std::string tmp = str;
			constexpr char finalDelim = ',';

			for (auto delim : delims) {
				std::ranges::replace(tmp, delim, finalDelim);  // replace all 'x' to 'y'
			}

			return split(tmp, finalDelim, RemoveEmpty);
		}

		static std::string trim(std::string_view s)
		{
			s.remove_prefix(std::min(s.find_first_not_of("\"\r\v\n"), s.size()));
			s.remove_suffix(std::min(s.size() - s.find_last_not_of("\"\r\v\n") - 1, s.size()));

			return std::string{ s };
		}
	};
}
