//----------------------------------------------------------------------------------------------------------------------
/// @file JsonWriter.cpp
/// @brief Implements the streaming JSON writer: separators, string escaping and round-trip number text.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/JsonWriter.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>

namespace rojoRHI::bench {
    //==================================================================================================================
    JsonWriter& JsonWriter::beginObject()
    {
        separate();
        m_text += '{';
        m_hasValue.push_back(false);
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::endObject()
    {
        m_hasValue.pop_back();
        m_text += '}';
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::beginArray()
    {
        separate();
        m_text += '[';
        m_hasValue.push_back(false);
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::endArray()
    {
        m_hasValue.pop_back();
        m_text += ']';
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::key(std::string_view name)
    {
        separate();
        appendEscaped(name);
        m_text += ':';
        m_afterKey = true;
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::string(std::string_view value)
    {
        separate();
        appendEscaped(value);
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::integer(int64_t value)
    {
        separate();
        std::array<char, 24> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        m_text.append(buffer.data(), result.ptr);
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::unsignedInteger(uint64_t value)
    {
        separate();
        std::array<char, 24> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        m_text.append(buffer.data(), result.ptr);
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::number(double value)
    {
        if (!std::isfinite(value))
        {
            return null();
        }
        separate();
        std::array<char, 32> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        m_text.append(buffer.data(), result.ptr);
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::boolean(bool value)
    {
        separate();
        m_text += value ? "true" : "false";
        return *this;
    }

    //==================================================================================================================
    JsonWriter& JsonWriter::null()
    {
        separate();
        m_text += "null";
        return *this;
    }

    //==================================================================================================================
    const std::string& JsonWriter::text() const
    {
        return m_text;
    }

    //==================================================================================================================
    std::expected<void, std::string> JsonWriter::save(const std::filesystem::path& path) const
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr)
        {
            return std::unexpected(std::format("json: cannot open {} for writing", path.string()));
        }
        const bool written = std::fwrite(m_text.data(), 1, m_text.size(), file) == m_text.size() && std::fputc('\n', file) != EOF;
        const bool closed = std::fclose(file) == 0;
        if (!written || !closed)
        {
            return std::unexpected(std::format("json: cannot write {}", path.string()));
        }
        return {};
    }

    //==================================================================================================================
    void JsonWriter::separate()
    {
        if (m_afterKey)
        {
            m_afterKey = false;
            return;
        }
        if (m_hasValue.empty())
        {
            return;
        }
        if (m_hasValue.back())
        {
            m_text += ',';
        }
        m_hasValue.back() = true;
    }

    //==================================================================================================================
    void JsonWriter::appendEscaped(std::string_view value)
    {
        m_text += '"';
        for (const char c : value)
        {
            switch (c)
            {
            case '"': m_text += "\\\""; break;
            case '\\': m_text += "\\\\"; break;
            case '\b': m_text += "\\b"; break;
            case '\f': m_text += "\\f"; break;
            case '\n': m_text += "\\n"; break;
            case '\r': m_text += "\\r"; break;
            case '\t': m_text += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    std::array<char, 8> escape{};
                    std::snprintf(escape.data(), escape.size(), "\\u%04x", static_cast<unsigned>(c));
                    m_text += escape.data();
                }
                else
                {
                    m_text += c;
                }
            }
        }
        m_text += '"';
    }
} // namespace rojoRHI::bench
