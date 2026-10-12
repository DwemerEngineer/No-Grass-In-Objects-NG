#pragma once

#include <cstdint>

#include <RE/B/BSCoreTypes.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RE
{
	class NiAVObject;

	class NiPoint3;
}

namespace GrassControl::Util::Files
{
	// Logical paths only: resolving backing paths would bypass mod-manager mappings.
	std::filesystem::path GamePath(const std::filesystem::path& relative);

	std::string PathForLog(const std::filesystem::path& path);

	bool ReadText(const std::filesystem::path& path, std::string& text, std::error_code& error);

	bool CreateText(const std::filesystem::path& path, std::string_view text, std::error_code& error);

	bool UpdateText(const std::filesystem::path& path, const std::function<bool(std::string&)>& update, std::error_code& error);

	bool IsMissing(const std::error_code& error);

	void SortUniquePaths(std::vector<std::filesystem::path>& paths);

	std::vector<std::filesystem::path> ObjectConfigs();
}

namespace GrassControl::Util
{
	inline std::string_view TrimView(std::string_view value)
	{
		constexpr auto whitespace = " \t\r\n\v\f\"";
		const auto first = value.find_first_not_of(whitespace);
		if (first == std::string_view::npos) {
			return {};
		}

		return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
	}

	struct FormSpec
	{
		std::uint32_t id;
		std::string filename;
	};

	inline std::optional<FormSpec> ParseFormSpec(std::string_view value)
	{
		value = TrimView(value);
		const auto separator = value.find(':');
		if (separator == std::string_view::npos) {
			return std::nullopt;
		}

		auto idText = TrimView(value.substr(0, separator));
		const auto filename = TrimView(value.substr(separator + 1));
		if (idText.starts_with("0x") || idText.starts_with("0X")) {
			idText.remove_prefix(2);
		}

		if (idText.empty() || filename.empty()) {
			return std::nullopt;
		}

		std::uint32_t id = 0;
		const auto [end, error] = std::from_chars(idText.data(), idText.data() + idText.size(), id, 16);
		if (error != std::errc{} || end != idText.data() + idText.size()) {
			return std::nullopt;
		}

		return FormSpec{ id, std::string(filename) };
	}

	inline std::optional<unsigned> ParseCollisionLayer(std::string_view value)
	{
		if (value.empty()) {
			return std::nullopt;
		}

		unsigned layer = 0;
		const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), layer);
		if (error != std::errc{} || end != value.data() + value.size() || layer >= 64) {
			return std::nullopt;
		}

		return layer;
	}

	inline bool MatchesShape(std::string_view name, std::string_view rootName, const std::unordered_set<std::string>& patterns)
	{
		for (const auto& pattern : patterns) {
			if (name == pattern || name == std::string(rootName) + ":" + pattern) {
				return true;
			}
		}

		return false;
	}

	inline bool IsShapeAllowed(std::string_view name, std::string_view rootName,
		const std::unordered_set<std::string>& allowed, const std::unordered_set<std::string>& blocked)
	{
		return (allowed.empty() || MatchesShape(name, rootName, allowed)) && !MatchesShape(name, rootName, blocked);
	}
}

namespace GrassControl::Util::CaseInsensitiveString
{
	inline constexpr unsigned char Fold(unsigned char c) noexcept
	{
		return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
	}

	struct Equal
	{
		bool operator()(std::string_view left, std::string_view right) const noexcept
		{
			if (left.size() != right.size()) {
				return false;
			}

			for (std::size_t i = 0; i < left.size(); ++i) {
				if (Fold(static_cast<unsigned char>(left[i])) != Fold(static_cast<unsigned char>(right[i]))) {
					return false;
				}
			}

			return true;
		}
	};

	struct Hash
	{
		std::size_t operator()(std::string_view value) const noexcept
		{
			std::size_t hash = 14695981039346656037ull;
			for (unsigned char c : value) {
				hash = (hash ^ Fold(c)) * 1099511628211ull;
			}

			return hash;
		}
	};
}

namespace GrassControl::Util
{
	void ReportAndFailTimed(const std::string& a_message);

	std::filesystem::path GetProgressFilePath();

	// Caller retains the attached root and holds the renderer lock.
	std::optional<std::string> ClosestShape(RE::NiAVObject* root, const RE::NiPoint3& position);

	class CachedFormList final
	{
	public:
		static std::unique_ptr<CachedFormList> TryParse(const std::string& input, const std::string& settingNameForLog);

		void printList(const std::string& settingNameForLog) const;

		bool Contains(RE::FormID formId) const { return Ids.contains(formId); }

		bool Empty() const { return Ids.empty(); }

		const std::unordered_set<RE::FormID>& GetAllFormIDs() const { return Ids; }

	private:
		std::unordered_set<RE::FormID> Ids;
	};
}

namespace GrassControl::Util::StringHelpers
{
	template <typename Range, typename Value = typename Range::value_type>
	inline std::string Join(Range const& elements, const char* const delimiter)
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

	inline std::vector<std::string> split(const std::string& str, char delim, const bool RemoveEmpty)
	{
		std::stringstream ss(str);
		std::string token;
		std::vector<std::string> tokens;

		while (getline(ss, token, delim)) {
			if (!RemoveEmpty || !token.empty())
				tokens.push_back(std::move(token));
		}

		return tokens;
	}

	inline std::vector<std::string> Split_at_any(const std::string& str, const std::vector<char>& delims, const bool RemoveEmpty)
	{
		std::string tmp = str;
		constexpr char finalDelim = ',';

		for (auto delim : delims) {
			std::ranges::replace(tmp, delim, finalDelim);
		}

		return split(tmp, finalDelim, RemoveEmpty);
	}

	inline std::string trim(std::string_view s)
	{
		return std::string(TrimView(s));
	}
}
