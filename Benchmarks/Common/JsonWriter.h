//----------------------------------------------------------------------------------------------------------------------
/// @file JsonWriter.h
/// @brief Declares the streaming JSON writer behind every JSON file the benchmark harness writes.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // Appends compact JSON to an in-memory text. A value written directly after key() takes no separator; every other
    // value or key inside an object or array is separated from the one before it. The caller writes a well-formed
    // document: the writer tracks separators, not grammar.
    class JsonWriter
    {
      public:
        JsonWriter& beginObject();
        JsonWriter& endObject();
        JsonWriter& beginArray();
        JsonWriter& endArray();
        JsonWriter& key(std::string_view name);
        JsonWriter& string(std::string_view value);
        JsonWriter& integer(int64_t value);
        JsonWriter& unsignedInteger(uint64_t value);
        // The shortest text that reads back as the same double; NaN and the infinities have no JSON form and write null.
        JsonWriter& number(double value);
        JsonWriter& boolean(bool value);
        JsonWriter& null();

        const std::string& text() const;
        // Writes the text and a final newline to `path`, replacing the file.
        std::expected<void, std::string> save(const std::filesystem::path& path) const;

      private:
        void separate();
        void appendEscaped(std::string_view value);

        std::string m_text;
        std::vector<bool> m_hasValue; ///< One entry per open object or array: whether it holds a value yet.
        bool m_afterKey = false;
    };
} // namespace rojoRHI::bench
