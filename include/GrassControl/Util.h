#pragma once

namespace GrassControl::Util
{
	namespace Files
	{
		// Logical paths only: resolving backing paths would bypass mod-manager mappings.
		inline std::filesystem::path GamePath(const std::filesystem::path& relative)
		{
			static const auto root = [] {
				std::wstring executable(260, L'\0');

				while (true) {
					const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
					if (length == 0) {
						throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot locate the game executable");
					}

					if (length < executable.size()) {
						executable.resize(length);
						return std::filesystem::path(executable).parent_path();
					}

					if (executable.size() >= 32768) {
						throw std::system_error(std::make_error_code(std::errc::filename_too_long), "Game executable path is too long");
					}

					executable.resize(std::min<std::size_t>(executable.size() * 2, 32768));
				}
			}();
			return (root / relative).lexically_normal().make_preferred();
		}

		std::string PathForLog(const std::filesystem::path& path);

		bool ReadText(const std::filesystem::path& path, std::string& text, std::error_code& error);

		bool CreateText(const std::filesystem::path& path, std::string_view text, std::error_code& error);

		bool UpdateText(const std::filesystem::path& path, const std::function<bool(std::string&)>& update, std::error_code& error);

		bool IsMissing(const std::error_code& error);

		void SortUniquePaths(std::vector<std::filesystem::path>& paths);

		std::vector<std::filesystem::path> ObjectConfigs();
	}

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

	namespace CaseInsensitiveString
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

	void ReportAndFailTimed(const std::string& a_message);

	inline std::filesystem::path GetProgressFilePath()
	{
		static const auto path = [] {
			const auto normal = Files::GamePath(L"PrecacheGrass.txt");
			const auto doubled = Files::GamePath(L"PrecacheGrass.txt.txt");
			std::error_code error;
			if (std::filesystem::is_regular_file(normal, error)) {
				return normal;
			}

			if ((!error || Files::IsMissing(error)) && std::filesystem::is_regular_file(doubled, error)) {
				return doubled;
			}

			if (error && !Files::IsMissing(error)) {
				logger::error("Failed to locate the progress file: {}", error.message());
			}

			return normal;
		}();
		return path;
	}

	inline constexpr std::size_t MaxSceneObjects = 4096;
	inline constexpr std::size_t MaxSceneVertices = 262144;

	// Captured vertices are in world space and remain usable after the scene graph is unlocked.
	struct ShapeSnapshot
	{
		struct Shape
		{
			std::string name;
			std::vector<RE::NiPoint3> vertices;
		};

		std::string root;
		std::vector<Shape> shapes;
	};

	// Caller retains the attached root and holds the renderer lock.
	std::optional<ShapeSnapshot> CaptureShapes(RE::NiAVObject* root);

	const ShapeSnapshot::Shape* ClosestShape(const ShapeSnapshot& snapshot, const RE::NiPoint3& position);

	class CachedFormList final
	{
	public:
		static std::unique_ptr<CachedFormList> TryParse(const std::string& input, const std::string& settingNameForLog);

		void PrintList(const std::string& settingNameForLog) const;

		bool Contains(RE::FormID formId) const { return Ids.contains(formId); }

		bool Empty() const { return Ids.empty(); }

		const std::unordered_set<RE::FormID>& GetAllFormIDs() const { return Ids; }

	private:
		std::unordered_set<RE::FormID> Ids;
	};

	namespace StringHelpers
	{
		inline std::vector<std::string> Split(std::string_view str, char delim, bool removeEmpty)
		{
			std::vector<std::string> tokens;
			for (auto part : str | std::views::split(delim)) {
				if (!removeEmpty || !part.empty())
					tokens.emplace_back(part.begin(), part.end());
			}
			return tokens;
		}

		inline std::vector<std::string> SplitAtAny(const std::string& str, const std::vector<char>& delims, bool removeEmpty)
		{
			std::string tmp = str;
			constexpr char finalDelim = ',';

			for (auto delim : delims) {
				std::ranges::replace(tmp, delim, finalDelim);
			}

			return Split(tmp, finalDelim, removeEmpty);
		}

		inline std::string Trim(std::string_view s)
		{
			return std::string(TrimView(s));
		}
	}
}
