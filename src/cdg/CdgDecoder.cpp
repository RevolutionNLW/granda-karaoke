#include "cdg/CdgDecoder.h"

#include <algorithm>
#include <utility>

namespace cdg {

namespace {

// Subcode command for CD+G graphics (the low 6 bits of byte 0).
constexpr std::uint8_t kCommandCdg = 0x09;

// CD+G instructions (the low 6 bits of byte 1).
constexpr std::uint8_t kMemoryPreset = 1;
constexpr std::uint8_t kBorderPreset = 2;
constexpr std::uint8_t kTileBlockNormal = 6;
constexpr std::uint8_t kScrollPreset = 20;
constexpr std::uint8_t kScrollCopy = 24;
constexpr std::uint8_t kDefineTransparent = 28;
constexpr std::uint8_t kLoadColorsLow = 30;
constexpr std::uint8_t kLoadColorsHigh = 31;
constexpr std::uint8_t kTileBlockXor = 38;

constexpr int kDataOffset = 4;  // 16 data bytes follow command, instruction, 2 parity bytes
constexpr int kBorderLeft = CdgDecoder::kTileWidth;
constexpr int kBorderRight = CdgDecoder::kWidth - CdgDecoder::kTileWidth;
constexpr int kBorderTop = CdgDecoder::kTileHeight;
constexpr int kBorderBottom = CdgDecoder::kHeight - CdgDecoder::kTileHeight;

constexpr bool isBorder(int x, int y)
{
    return x < kBorderLeft || x >= kBorderRight || y < kBorderTop || y >= kBorderBottom;
}

} // namespace

CdgDecoder::CdgDecoder()
{
    reset();
}

void CdgDecoder::setData(std::vector<std::uint8_t> data)
{
    m_data = std::move(data);
    reset();
}

void CdgDecoder::clear()
{
    m_data.clear();
    reset();
}

void CdgDecoder::reset()
{
    m_nextPacket = 0;
    m_skipped = 0;
    m_screen.fill(0);
    m_palette.fill(0xFF000000u);
    m_hOffset = 0;
    m_vOffset = 0;
    ++m_revision;
}

std::size_t CdgDecoder::packetsDueAt(std::int64_t positionMs)
{
    if (positionMs <= 0)
        return 0;
    return static_cast<std::size_t>(positionMs * kPacketsPerSecond / 1000);
}

std::int64_t CdgDecoder::durationMs() const
{
    return static_cast<std::int64_t>(packetCount()) * 1000 / kPacketsPerSecond;
}

std::size_t CdgDecoder::graphicsPacketCount() const
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < packetCount(); ++i) {
        if ((m_data[i * kPacketSize] & 0x3F) == kCommandCdg)
            ++count;
    }
    return count;
}

bool CdgDecoder::advanceTo(std::int64_t positionMs)
{
    const std::size_t target = std::min(packetsDueAt(positionMs), packetCount());

    bool rewound = false;
    if (target < m_nextPacket) {
        reset();
        rewound = true;
    }

    const bool advanced = target > m_nextPacket;
    while (m_nextPacket < target) {
        applyPacket(&m_data[m_nextPacket * kPacketSize]);
        ++m_nextPacket;
    }
    return advanced || rewound;
}

void CdgDecoder::applyPacket(const std::uint8_t* packet)
{
    if ((packet[0] & 0x3F) != kCommandCdg)
        return;  // Not a graphics packet (or empty subcode); nothing to do.

    const std::uint8_t instruction = packet[1] & 0x3F;
    std::uint8_t data[16];
    for (int i = 0; i < 16; ++i)
        data[i] = packet[kDataOffset + i] & 0x3F;

    switch (instruction) {
    case kMemoryPreset:
        memoryPreset(data);
        break;
    case kBorderPreset:
        borderPreset(data);
        break;
    case kTileBlockNormal:
        tileBlock(data, false);
        break;
    case kTileBlockXor:
        tileBlock(data, true);
        break;
    case kScrollPreset:
        scroll(data, false);
        break;
    case kScrollCopy:
        scroll(data, true);
        break;
    case kLoadColorsLow:
        loadColors(data, 0);
        break;
    case kLoadColorsHigh:
        loadColors(data, 8);
        break;
    case kDefineTransparent:
        break;  // Only meaningful for video overlay; not needed here.
    default:
        ++m_skipped;
        break;
    }
}

void CdgDecoder::memoryPreset(const std::uint8_t* data)
{
    // Discs repeat this packet (data[1] is the repeat counter) for robustness;
    // re-applying it is harmless, so every copy is applied.
    m_screen.fill(data[0] & 0x0F);
    ++m_revision;
}

void CdgDecoder::borderPreset(const std::uint8_t* data)
{
    const std::uint8_t color = data[0] & 0x0F;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (isBorder(x, y))
                m_screen[y * kWidth + x] = color;
        }
    }
    ++m_revision;
}

void CdgDecoder::tileBlock(const std::uint8_t* data, bool xorMode)
{
    const std::uint8_t color0 = data[0] & 0x0F;
    const std::uint8_t color1 = data[1] & 0x0F;
    const int row = data[2] & 0x1F;
    const int column = data[3] & 0x3F;
    if (row >= kTileRows || column >= kTileColumns) {
        ++m_skipped;
        return;
    }

    const int left = column * kTileWidth;
    const int top = row * kTileHeight;
    for (int line = 0; line < kTileHeight; ++line) {
        const std::uint8_t bits = data[4 + line];
        for (int i = 0; i < kTileWidth; ++i) {
            const bool on = (bits >> (kTileWidth - 1 - i)) & 1;
            const std::uint8_t color = on ? color1 : color0;
            std::uint8_t& target = m_screen[(top + line) * kWidth + left + i];
            target = xorMode ? static_cast<std::uint8_t>((target ^ color) & 0x0F) : color;
        }
    }
    ++m_revision;
}

void CdgDecoder::scroll(const std::uint8_t* data, bool copy)
{
    const std::uint8_t fill = data[0] & 0x0F;
    const int hCommand = (data[1] & 0x30) >> 4;
    const int vCommand = (data[2] & 0x30) >> 4;
    // Offsets outside the documented ranges are clamped rather than rejected.
    m_hOffset = std::min(data[1] & 0x07, kTileWidth - 1);
    m_vOffset = std::min(data[2] & 0x0F, kTileHeight - 1);

    // 1 = move content right/down, 2 = move content left/up.
    int dx = 0;
    int dy = 0;
    if (hCommand == 1)
        dx = kTileWidth;
    else if (hCommand == 2)
        dx = -kTileWidth;
    if (vCommand == 1)
        dy = kTileHeight;
    else if (vCommand == 2)
        dy = -kTileHeight;

    if (dx != 0 || dy != 0) {
        const auto old = m_screen;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                int sx = x - dx;
                int sy = y - dy;
                const bool outside = sx < 0 || sx >= kWidth || sy < 0 || sy >= kHeight;
                if (outside && !copy) {
                    m_screen[y * kWidth + x] = fill;
                    continue;
                }
                sx = (sx + kWidth) % kWidth;
                sy = (sy + kHeight) % kHeight;
                m_screen[y * kWidth + x] = old[sy * kWidth + sx];
            }
        }
    }
    ++m_revision;
}

void CdgDecoder::loadColors(const std::uint8_t* data, int firstIndex)
{
    for (int i = 0; i < 8; ++i) {
        const std::uint8_t high = data[2 * i];
        const std::uint8_t low = data[2 * i + 1];
        const std::uint32_t red = (high & 0x3C) >> 2;
        const std::uint32_t green = ((high & 0x03) << 2) | ((low & 0x30) >> 4);
        const std::uint32_t blue = low & 0x0F;
        // Scale 4-bit components to 8 bits (0x0 -> 0x00, 0xF -> 0xFF).
        m_palette[firstIndex + i] = 0xFF000000u | (red * 17) << 16 | (green * 17) << 8 | (blue * 17);
    }
    ++m_revision;
}

void CdgDecoder::renderArgb32(std::uint32_t* out, int strideInPixels) const
{
    for (int y = 0; y < kHeight; ++y) {
        std::uint32_t* line = out + static_cast<std::ptrdiff_t>(y) * strideInPixels;
        for (int x = 0; x < kWidth; ++x) {
            // The display window inside the border is shifted by the scroll
            // offsets; the border itself is shown as-is.
            const int sx = isBorder(x, y) ? x : x + m_hOffset;
            const int sy = isBorder(x, y) ? y : y + m_vOffset;
            line[x] = m_palette[m_screen[sy * kWidth + sx]];
        }
    }
}

} // namespace cdg
