#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cdg {

// Decodes a CD+G subcode stream into a 300x216 indexed-colour screen.
//
// The decoder has no clock of its own. The caller supplies the playback
// position (the audio position is the master clock) and the decoder applies,
// in order, every packet whose time has been reached. Moving the position
// backwards replays from the start so the screen is always exactly the state
// the stream defines for that position.
//
// Malformed input never throws: a trailing partial packet is ignored and
// instructions with out-of-range parameters are skipped and counted.
class CdgDecoder {
public:
    static constexpr int kWidth = 300;
    static constexpr int kHeight = 216;
    static constexpr int kPacketSize = 24;
    static constexpr int kPacketsPerSecond = 300;
    static constexpr int kTileWidth = 6;
    static constexpr int kTileHeight = 12;
    static constexpr int kTileColumns = kWidth / kTileWidth;  // 50
    static constexpr int kTileRows = kHeight / kTileHeight;   // 18

    CdgDecoder();

    // Replaces the stream and resets the screen to its initial state.
    void setData(std::vector<std::uint8_t> data);

    // Drops the stream and resets.
    void clear();

    // Resets the screen, palette and position to the start of the stream.
    // The stream itself is kept.
    void reset();

    // Applies all packets due at positionMs. Returns true if any packet was
    // applied (the screen may have changed).
    bool advanceTo(std::int64_t positionMs);

    std::size_t packetCount() const { return m_data.size() / kPacketSize; }
    std::size_t graphicsPacketCount() const;
    std::size_t packetsApplied() const { return m_nextPacket; }
    std::size_t trailingBytes() const { return m_data.size() % kPacketSize; }
    std::size_t skippedInstructions() const { return m_skipped; }
    std::int64_t durationMs() const;

    // Increments whenever an instruction modifies the screen or palette.
    std::uint64_t revision() const { return m_revision; }

    std::uint8_t pixel(int x, int y) const { return m_screen[y * kWidth + x]; }
    std::uint32_t paletteColor(int index) const { return m_palette[index]; }
    int horizontalOffset() const { return m_hOffset; }
    int verticalOffset() const { return m_vOffset; }

    // Writes the visible 300x216 image as 0xAARRGGBB pixels.
    // strideInPixels is the distance between rows in the output buffer.
    void renderArgb32(std::uint32_t* out, int strideInPixels) const;

    // Number of packets that are due by the given position.
    static std::size_t packetsDueAt(std::int64_t positionMs);

private:
    void applyPacket(const std::uint8_t* packet);
    void memoryPreset(const std::uint8_t* data);
    void borderPreset(const std::uint8_t* data);
    void tileBlock(const std::uint8_t* data, bool xorMode);
    void scroll(const std::uint8_t* data, bool copy);
    void loadColors(const std::uint8_t* data, int firstIndex);

    std::vector<std::uint8_t> m_data;
    std::size_t m_nextPacket = 0;
    std::size_t m_skipped = 0;
    std::uint64_t m_revision = 0;
    std::array<std::uint8_t, kWidth * kHeight> m_screen{};
    std::array<std::uint32_t, 16> m_palette{};
    int m_hOffset = 0;
    int m_vOffset = 0;
};

} // namespace cdg
