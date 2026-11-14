#include "analysis.hpp"
#include <algorithm>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {
using hg::Text;
using Bytes = std::vector<uint8_t>;

Bytes bytes(std::string_view s) {
    return {s.begin(), s.end()};
}

void expect(std::span<const uint8_t> input, size_t minimum, std::string_view encoding,
            std::initializer_list<Text> expected, size_t alignment = 2) {
    const auto all = hg::extract_strings(input, minimum);
    std::vector<Text> actual;
    std::copy_if(all.begin(), all.end(), std::back_inserter(actual), [&](const auto &text) {
        return text.encoding == encoding && (alignment > 1 || text.offset % 2 == alignment);
    });
    if (actual.size() != expected.size() ||
        !std::equal(actual.begin(), actual.end(), expected.begin(), [](const auto &a, const auto &e) {
            return a.offset == e.offset && a.encoding == e.encoding && a.value == e.value;
        })) {
        std::string details;
        for (const auto &text : actual)
            details += " [" + std::to_string(text.offset) + ": " + text.value + "]";
        throw std::runtime_error("Unexpected " + std::string(encoding) + " extraction:" + details);
    }
}

void ascii_compatibility() {
    const Bytes input{0, 'a', 'b', 'c', 'd', 0, 'x', 'y', 127, 'z'};
    expect(input, 4, "ASCII", {{1, "ASCII", "abcd"}});
    expect(input, 0, "ASCII", {{1, "ASCII", "abcd"}, {6, "ASCII", "xy"}});
    expect(input, 2, "UTF-8", {});
    expect(bytes("abé世界"), 2, "ASCII", {{0, "ASCII", "ab"}});
    expect(bytes("abé世界"), 5, "UTF-8", {{0, "UTF-8", "abé世界"}});
    expect({}, 2, "ASCII", {});
}

void utf8_text_and_lengths() {
    const Bytes input{0, 0xd7, 0xa9, 0xd7, 0x9c, 0xd7, 0x95, 0xd7, 0x9d,
                      0, 0xe4, 0xb8, 0x96, 0xe7, 0x95, 0x8c, 0};
    expect(input, 2, "UTF-8", {{1, "UTF-8", "שלום"}, {10, "UTF-8", "世界"}});
    expect(input, 3, "UTF-8", {{1, "UTF-8", "שלום"}});
    expect(bytes("é😀"), 2, "UTF-8", {{0, "UTF-8", "é😀"}});
    expect(bytes("é😀"), 3, "UTF-8", {});
    expect(bytes("𠀀界"), 2, "UTF-8", {{0, "UTF-8", "𠀀界"}});
    expect(bytes("aé\nbé"), 2, "UTF-8", {{0, "UTF-8", "aé"}, {4, "UTF-8", "bé"}});
}

void malformed_utf8() {
    const std::vector<Bytes> invalid{
        {0xc0, 0xaf},             // Overlong two-byte sequence.
        {0xe0, 0x80, 0xaf},       // Overlong three-byte sequence.
        {0xf0, 0x80, 0x80, 0xaf}, // Overlong four-byte sequence.
        {0xed, 0xa0, 0x80},       // Surrogate scalar.
        {0xf4, 0x90, 0x80, 0x80}, // Above U+10FFFF.
        {0x80, 0xbf},             // Stray continuations.
        {0xef, 0xb7, 0x90},       // U+FDD0 noncharacter.
        {0xf0, 0x9f, 0xbf, 0xbe}, // U+1FFFE noncharacter.
        {0xc2, 0x85},             // U+0085 control.
        {0xe2, 0x80, 0xae},       // Bidi format control.
        {0xcd, 0xb8},             // Unassigned U+0378.
        {0xee, 0x80, 0x80},       // Private-use U+E000.
        {0xe2, 0x82},             // Truncated three-byte sequence.
        {0xf0, 0x9f, 0x98},       // Truncated four-byte sequence.
        {0xc2},                   // Truncated two-byte sequence.
    };
    for (auto input : invalid) {
        const auto offset = input.size();
        const auto tail = bytes("éZ");
        input.insert(input.end(), tail.begin(), tail.end());
        expect(input, 2, "UTF-8", {{offset, "UTF-8", "éZ"}});
    }
    expect(Bytes{0xc3, 'X', 0xc3, 0xa9, 'Z', 0xf0, 0x9f}, 2, "UTF-8", {{1, "UTF-8", "XéZ"}});
}

void utf16_text_and_lengths() {
    // Hebrew starts at even offset, CJK starts at odd offset.
    const Bytes even{0, 0, 0xe9, 5, 0xdc, 5, 0xd5, 5, 0xdd, 5, 0, 0};
    const Bytes odd{0, 0x16, 0x4e, 0x4c, 0x75, 0, 0};
    expect(even, 4, "UTF-16LE", {{2, "UTF-16LE", "שלום"}}, 0);
    expect(odd, 2, "UTF-16LE", {{1, "UTF-16LE", "世界"}}, 1);
    expect(odd, 3, "UTF-16LE", {}, 1);
    const Bytes emoji{0x3d, 0xd8, 0, 0xde, 0x3d, 0xd8, 1, 0xde, 0, 0};
    expect(emoji, 2, "UTF-16LE", {{0, "UTF-16LE", "😀😁"}}, 0);
    expect(emoji, 3, "UTF-16LE", {}, 0);
    auto odd_emoji = emoji;
    odd_emoji.insert(odd_emoji.begin(), 0);
    expect(odd_emoji, 2, "UTF-16LE", {{1, "UTF-16LE", "😀😁"}}, 1);
    const Bytes ascii{0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 0, 0};
    expect(ascii, 4, "UTF-16LE", {{1, "UTF-16LE", "abcd"}}, 1);
}

void malformed_utf16() {
    const std::vector<Bytes> invalid{
        {0, 0xdc},                // Isolated low surrogate.
        {0, 0xd8},                // High surrogate followed by a non-surrogate.
        {0xff, 0xff},             // BMP noncharacter.
        {0x3f, 0xd8, 0xfe, 0xdf}, // Supplementary noncharacter U+1FFFE.
        {0x85, 0},                // Control.
        {0x2e, 0x20},             // Bidi format control U+202E.
        {0x78, 3},                // Unassigned U+0378.
        {0, 0xe0},                // Private use.
    };
    for (auto input : invalid) {
        const auto offset = input.size();
        input.insert(input.end(), {0xe9, 5, 0xdc, 5, 0, 0});
        expect(input, 2, "UTF-16LE", {{offset, "UTF-16LE", "של"}}, 0);
    }
    expect(Bytes{0xe9, 5, 0xdc, 5, 0, 0xd8}, 2, "UTF-16LE", {{0, "UTF-16LE", "של"}}, 0);
    expect(Bytes{0xe9, 5, 0xdc, 5, 0x3d}, 2, "UTF-16LE", {{0, "UTF-16LE", "של"}}, 0);
}
} // namespace

int main() {
    try {
        ascii_compatibility();
        utf8_text_and_lengths();
        malformed_utf8();
        utf16_text_and_lengths();
        malformed_utf16();
        std::cout << "String extraction tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
