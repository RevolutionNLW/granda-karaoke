#pragma once

// Builders for synthetic CD+G packets used by the tests.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <vector>

namespace testcdg {

using Packet = std::array<std::uint8_t, 24>;

inline Packet instruction(std::uint8_t code, std::initializer_list<std::uint8_t> data)
{
    Packet p{};
    p[0] = 0x09;  // CD+G command
    p[1] = code;
    std::size_t i = 4;
    for (std::uint8_t b : data)
        p[i++] = b;
    return p;
}

inline Packet memoryPreset(std::uint8_t color) { return instruction(1, {color, 0}); }
inline Packet borderPreset(std::uint8_t color) { return instruction(2, {color}); }

// rows: 12 six-bit rows, most significant bit = leftmost pixel.
inline Packet tile(std::uint8_t color0, std::uint8_t color1, std::uint8_t row, std::uint8_t column,
                   const std::array<std::uint8_t, 12>& rows, bool xorMode = false)
{
    Packet p = instruction(xorMode ? 38 : 6, {color0, color1, row, column});
    for (std::size_t i = 0; i < 12; ++i)
        p[8 + i] = rows[i];
    return p;
}

inline Packet solidTile(std::uint8_t color, std::uint8_t row, std::uint8_t column)
{
    std::array<std::uint8_t, 12> rows{};
    rows.fill(0x3F);
    return tile(0, color, row, column, rows);
}

// colors: eight 12-bit 0xRGB values.
inline Packet loadColors(bool high, const std::array<std::uint16_t, 8>& colors)
{
    Packet p = instruction(high ? 31 : 30, {});
    for (std::size_t i = 0; i < 8; ++i) {
        const std::uint8_t r = (colors[i] >> 8) & 0xF;
        const std::uint8_t g = (colors[i] >> 4) & 0xF;
        const std::uint8_t b = colors[i] & 0xF;
        p[4 + 2 * i] = static_cast<std::uint8_t>((r << 2) | (g >> 2));
        p[5 + 2 * i] = static_cast<std::uint8_t>(((g & 0x3) << 4) | b);
    }
    return p;
}

inline Packet scroll(bool copy, std::uint8_t color, int hCommand, int hOffset, int vCommand, int vOffset)
{
    return instruction(copy ? 24 : 20,
                       {color, static_cast<std::uint8_t>((hCommand << 4) | hOffset),
                        static_cast<std::uint8_t>((vCommand << 4) | vOffset)});
}

// Builds a stream of totalPackets packets, empty except where given. A packet
// placed at or after the end is not part of the stream.
inline std::vector<std::uint8_t> stream(std::size_t totalPackets, const std::map<std::size_t, Packet>& packets)
{
    std::vector<std::uint8_t> data(totalPackets * 24, 0);
    for (const auto& [index, packet] : packets) {
        if (index >= totalPackets)
            break;  // the map is ordered: every later packet is past the end too
        for (std::size_t i = 0; i < 24; ++i)
            data[index * 24 + i] = packet[i];
    }
    return data;
}

// A stream that shows a text-like title screen from the start (tiles drawn
// in `firstColumn`.. on two rows), clears it at `clearSecond`, then draws a
// lyric line. Different `firstColumn` values give different content.
inline std::vector<std::uint8_t> titleScreenStream(std::uint8_t firstColumn, int clearSecond = 5,
                                                   int totalSeconds = 12)
{
    std::map<std::size_t, Packet> packets;
    packets[0] = loadColors(false, {0x000, 0xFFF, 0xF00, 0x0F0, 0x00F, 0xFF0, 0x0FF, 0xF0F});
    packets[1] = memoryPreset(0);
    std::array<std::uint8_t, 12> glyph{};
    for (std::size_t i = 0; i < glyph.size(); ++i)
        glyph[i] = (i % 2) ? 0x2A : 0x15;
    std::size_t index = 2;
    for (std::uint8_t row = 7; row <= 8; ++row) {
        for (std::uint8_t column = firstColumn; column < firstColumn + 20; ++column)
            packets[index++] = tile(0, 1, row, column, glyph);
    }
    const std::size_t clear = static_cast<std::size_t>(clearSecond) * 300;
    packets[clear] = memoryPreset(0);
    index = clear + 300;
    for (std::uint8_t column = 5; column < 35; ++column)
        packets[index++] = tile(0, 2, 14, column, glyph);
    return stream(static_cast<std::size_t>(totalSeconds) * 300, packets);
}

} // namespace testcdg
