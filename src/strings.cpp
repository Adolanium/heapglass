#include "analysis.hpp"
#include <algorithm>
#include <optional>

namespace hg {
namespace {
struct Scalar {
    uint32_t value;
    size_t bytes;
};

bool text_scalar(uint32_t c) {
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || (c >= 0xfdd0 && c <= 0xfdef) ||
        (c & 0xffff) >= 0xfffe)
        return false;
    // NLS classifies UTF-16 code units, not supplementary scalar assignment. Keep those candidates
    // to assigned CJK Extension B and common emoji ranges; this is deliberately not all Unicode.
    if (c > 0xffff)
        return (c >= 0x20000 && c <= 0x2a6df) || (c >= 0x1f600 && c <= 0x1f64f) ||
               (c >= 0x1f680 && c <= 0x1f6c5);
    // Exclude private use, formatting controls, and line/paragraph separators as run boundaries.
    if ((c >= 0xe000 && c <= 0xf8ff) || c == 0xad || (c >= 0x600 && c <= 0x605) || c == 0x61c || c == 0x6dd ||
        c == 0x70f || (c >= 0x890 && c <= 0x891) || c == 0x8e2 || c == 0x180e ||
        (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f) ||
        c == 0xfeff || (c >= 0xfff9 && c <= 0xfffb))
        return false;
    const wchar_t unit = static_cast<wchar_t>(c);
    WORD kind = 0;
    return GetStringTypeW(CT_CTYPE1, &unit, 1, &kind) && (kind & C1_DEFINED) && !(kind & C1_CNTRL);
}

std::optional<Scalar> read_utf8(std::span<const uint8_t> b, size_t i) {
    const auto lead = b[i];
    if (lead < 0x80)
        return Scalar{lead, 1};
    size_t count;
    uint32_t c, least;
    if (lead >= 0xc2 && lead <= 0xdf) {
        count = 2;
        c = lead & 0x1f;
        least = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        count = 3;
        c = lead & 0x0f;
        least = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        count = 4;
        c = lead & 7;
        least = 0x10000;
    } else
        return std::nullopt;
    if (b.size() - i < count)
        return std::nullopt;
    for (size_t j = 1; j < count; ++j) {
        if ((b[i + j] & 0xc0) != 0x80)
            return std::nullopt;
        c = (c << 6) | (b[i + j] & 0x3f);
    }
    if (c < least || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return std::nullopt;
    return Scalar{c, count};
}

uint32_t word(std::span<const uint8_t> b, size_t i) {
    return b[i] | (uint32_t(b[i + 1]) << 8);
}

std::optional<Scalar> read_utf16(std::span<const uint8_t> b, size_t i) {
    if (b.size() - i < 2)
        return std::nullopt;
    const auto c = word(b, i);
    if (c >= 0xd800 && c <= 0xdbff) {
        if (b.size() - i < 4)
            return std::nullopt;
        const auto low = word(b, i + 2);
        if (low < 0xdc00 || low > 0xdfff)
            return std::nullopt;
        return Scalar{0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00), 4};
    }
    if (c >= 0xdc00 && c <= 0xdfff)
        return std::nullopt;
    return Scalar{c, 2};
}

void append_utf8(std::string &s, uint32_t c) {
    if (c < 0x80)
        s += static_cast<char>(c);
    else if (c < 0x800) {
        s += static_cast<char>(0xc0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
        s += static_cast<char>(0xe0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 63));
        s += static_cast<char>(0x80 | (c & 63));
    } else {
        s += static_cast<char>(0xf0 | (c >> 18));
        s += static_cast<char>(0x80 | ((c >> 12) & 63));
        s += static_cast<char>(0x80 | ((c >> 6) & 63));
        s += static_cast<char>(0x80 | (c & 63));
    }
}
} // namespace

std::vector<Text> extract_strings(std::span<const uint8_t> b, size_t minimum) {
    minimum = std::max<size_t>(2, minimum);
    std::vector<Text> out;
    for (size_t i = 0; i < b.size();) {
        const auto start = i;
        while (i < b.size() && b[i] >= 32 && b[i] < 127)
            ++i;
        if (i - start >= minimum)
            out.push_back(
                {start, "ASCII", std::string(reinterpret_cast<const char *>(b.data() + start), i - start)});
        if (i == start)
            ++i;
    }
    for (size_t i = 0; i < b.size();) {
        const auto start = i;
        size_t count = 0;
        bool multibyte = false;
        while (i < b.size()) {
            const auto scalar = read_utf8(b, i);
            if (!scalar || !text_scalar(scalar->value))
                break;
            multibyte |= scalar->bytes > 1;
            i += scalar->bytes;
            ++count;
        }
        if (count >= minimum && multibyte)
            out.push_back(
                {start, "UTF-8", std::string(reinterpret_cast<const char *>(b.data() + start), i - start)});
        if (i == start)
            ++i;
    }
    // Both alignments can produce plausible candidates in binary data; no language is inferred.
    for (size_t alignment = 0; alignment < 2; ++alignment)
        for (size_t i = alignment; i < b.size();) {
            const auto start = i;
            size_t count = 0;
            std::string s;
            while (i < b.size()) {
                const auto scalar = read_utf16(b, i);
                if (!scalar || !text_scalar(scalar->value))
                    break;
                append_utf8(s, scalar->value);
                i += scalar->bytes;
                ++count;
            }
            if (count >= minimum)
                out.push_back({start, "UTF-16LE", std::move(s)});
            if (i == start)
                i += std::min<size_t>(2, b.size() - i);
        }
    return out;
}
} // namespace hg
