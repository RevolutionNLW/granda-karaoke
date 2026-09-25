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

// Builds a stream of totalPackets packets, empty except where given.
inline std::vector<std::uint8_t> stream(std::size_t totalPackets, const std::map<std::size_t, Packet>& packets)
{
    std::vector<std::uint8_t> data(totalPackets * 24, 0);
    for (const auto& [index, packet] : packets) {
        for (std::size_t i = 0; i < 24; ++i)
            data[index * 24 + i] = packet[i];
    }
    return data;
}

} // namespace testcdg
