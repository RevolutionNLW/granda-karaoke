#include "CdgTestData.h"
#include "cdg/CdgDecoder.h"

#include <QtTest>

using cdg::CdgDecoder;
using namespace testcdg;

namespace {

// Packet index whose time is exactly at the given millisecond (300 packets/s).
constexpr std::size_t at(int ms) { return static_cast<std::size_t>(ms) * 300 / 1000; }

bool screenEquals(const CdgDecoder& a, const CdgDecoder& b)
{
    for (int y = 0; y < CdgDecoder::kHeight; ++y)
        for (int x = 0; x < CdgDecoder::kWidth; ++x)
            if (a.pixel(x, y) != b.pixel(x, y))
                return false;
    for (int i = 0; i < 16; ++i)
        if (a.paletteColor(i) != b.paletteColor(i))
            return false;
    return a.horizontalOffset() == b.horizontalOffset() && a.verticalOffset() == b.verticalOffset();
}

} // namespace

class TestCdgDecoder : public QObject {
    Q_OBJECT

private slots:
    void initialStateIsBlank();
    void graphicsPacketCount();
    void memoryPresetFillsScreen();
    void borderPresetFillsOnlyBorder();
    void loadColorTables();
    void tileBlockDrawsPattern();
    void tileBlockXorCombines();
    void outOfRangeTileIsSkipped();
    void nonGraphicsPacketsAreIgnored();
    void unknownInstructionIsCounted();
    void packetsAppliedOnlyWhenTimeReached();
    void advanceIsClampedToStreamLength();
    void resetClearsStateButKeepsStream();
    void replayAfterResetMatchesFreshDecode();
    void rewindReplaysFromStart();
    void setDataReplacesStreamAndResets();
    void truncatedStreamIgnoresPartialPacket();
    void emptyAndTinyStreamsAreHarmless();
    void randomDataDoesNotCrash();
    void scrollPresetShiftsAndFills();
    void scrollCopyWraps();
    void scrollOffsetsAffectRendering();
    void renderUsesPalette();
};

void TestCdgDecoder::graphicsPacketCount()
{
    CdgDecoder d;
    QCOMPARE(d.graphicsPacketCount(), 0u);
    Packet masked = memoryPreset(1);
    masked[0] |= 0xC0;
    auto bytes = stream(4, {{1, masked}, {2, instruction(63, {})}});
    bytes.push_back(0x09);  // incomplete graphics packet must not count
    d.setData(bytes);
    QCOMPARE(d.graphicsPacketCount(), 2u);
    d.advanceTo(1000);
    d.reset();
    QCOMPARE(d.graphicsPacketCount(), 2u);
    d.setData(stream(5, {}));
    QCOMPARE(d.graphicsPacketCount(), 0u);
    d.clear();
    QCOMPARE(d.graphicsPacketCount(), 0u);
}

void TestCdgDecoder::initialStateIsBlank()
{
    CdgDecoder d;
    QCOMPARE(d.packetCount(), 0u);
    QCOMPARE(d.packetsApplied(), 0u);
    QCOMPARE(d.pixel(0, 0), 0);
    QCOMPARE(d.pixel(150, 100), 0);
    QCOMPARE(d.paletteColor(0), 0xFF000000u);
}

void TestCdgDecoder::memoryPresetFillsScreen()
{
    CdgDecoder d;
    d.setData(stream(1, {{0, memoryPreset(5)}}));
    QVERIFY(d.advanceTo(1000));
    QCOMPARE(d.pixel(0, 0), 5);
    QCOMPARE(d.pixel(299, 215), 5);
    QCOMPARE(d.pixel(150, 100), 5);
}

void TestCdgDecoder::borderPresetFillsOnlyBorder()
{
    CdgDecoder d;
    d.setData(stream(2, {{0, memoryPreset(1)}, {1, borderPreset(7)}}));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(0, 0), 7);
    QCOMPARE(d.pixel(5, 100), 7);     // left border
    QCOMPARE(d.pixel(294, 100), 7);   // right border
    QCOMPARE(d.pixel(150, 11), 7);    // top border
    QCOMPARE(d.pixel(150, 204), 7);   // bottom border
    QCOMPARE(d.pixel(6, 12), 1);      // first inner pixel
    QCOMPARE(d.pixel(293, 203), 1);   // last inner pixel
}

void TestCdgDecoder::loadColorTables()
{
    CdgDecoder d;
    d.setData(stream(2, {
        {0, loadColors(false, {0x000, 0xF00, 0x0F0, 0x00F, 0xFFF, 0x123, 0x456, 0x789})},
        {1, loadColors(true, {0xABC, 0xDEF, 0x111, 0x222, 0x333, 0x444, 0x555, 0x666})},
    }));
    d.advanceTo(1000);
    QCOMPARE(d.paletteColor(0), 0xFF000000u);
    QCOMPARE(d.paletteColor(1), 0xFFFF0000u);
    QCOMPARE(d.paletteColor(2), 0xFF00FF00u);
    QCOMPARE(d.paletteColor(3), 0xFF0000FFu);
    QCOMPARE(d.paletteColor(4), 0xFFFFFFFFu);
    QCOMPARE(d.paletteColor(5), 0xFF112233u);
    QCOMPARE(d.paletteColor(8), 0xFFAABBCCu);
    QCOMPARE(d.paletteColor(9), 0xFFDDEEFFu);
    QCOMPARE(d.paletteColor(15), 0xFF666666u);
}

void TestCdgDecoder::tileBlockDrawsPattern()
{
    std::array<std::uint8_t, 12> rows{};
    rows[0] = 0b100000;  // leftmost pixel of first line
    rows[11] = 0b000001; // rightmost pixel of last line
    CdgDecoder d;
    d.setData(stream(1, {{0, tile(2, 9, 3, 10, rows)}}));
    d.advanceTo(1000);
    const int left = 10 * 6;
    const int top = 3 * 12;
    QCOMPARE(d.pixel(left, top), 9);
    QCOMPARE(d.pixel(left + 1, top), 2);
    QCOMPARE(d.pixel(left + 5, top + 11), 9);
    QCOMPARE(d.pixel(left + 4, top + 11), 2);
    QCOMPARE(d.pixel(left - 1, top), 0);  // untouched outside the tile
    QCOMPARE(d.pixel(left + 6, top), 0);
}

void TestCdgDecoder::tileBlockXorCombines()
{
    std::array<std::uint8_t, 12> rows{};
    rows[0] = 0b110000;
    CdgDecoder d;
    d.setData(stream(2, {{0, solidTile(0b0101, 0, 0)}, {1, tile(0b0000, 0b0011, 0, 0, rows, true)}}));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(0, 0), 0b0110);  // 0101 ^ 0011
    QCOMPARE(d.pixel(1, 0), 0b0110);
    QCOMPARE(d.pixel(2, 0), 0b0101);  // 0101 ^ 0000
}

void TestCdgDecoder::outOfRangeTileIsSkipped()
{
    CdgDecoder d;
    d.setData(stream(2, {{0, solidTile(3, 18, 0)}, {1, solidTile(3, 0, 50)}}));
    d.advanceTo(1000);
    QCOMPARE(d.skippedInstructions(), 2u);
    QCOMPARE(d.pixel(0, 0), 0);
}

void TestCdgDecoder::nonGraphicsPacketsAreIgnored()
{
    Packet p = memoryPreset(4);
    p[0] = 0x08;  // not the CD+G command
    CdgDecoder d;
    d.setData(stream(1, {{0, p}}));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(10, 10), 0);
    QCOMPARE(d.skippedInstructions(), 0u);

    // High bits (P/Q subchannel) must be masked off.
    Packet masked = memoryPreset(4);
    masked[0] |= 0xC0;
    masked[1] |= 0xC0;
    masked[4] |= 0xC0;
    d.setData(stream(1, {{0, masked}}));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(10, 10), 4);
}

void TestCdgDecoder::unknownInstructionIsCounted()
{
    CdgDecoder d;
    d.setData(stream(2, {{0, instruction(63, {1, 2, 3})}, {1, memoryPreset(2)}}));
    d.advanceTo(1000);
    QCOMPARE(d.skippedInstructions(), 1u);
    QCOMPARE(d.pixel(0, 0), 2);  // later packets still applied
}

void TestCdgDecoder::packetsAppliedOnlyWhenTimeReached()
{
    // Tile at 1.000 s, memory preset at 2.000 s.
    CdgDecoder d;
    d.setData(stream(900, {{at(1000), solidTile(6, 1, 1)}, {at(2000), memoryPreset(3)}}));

    QVERIFY(!d.advanceTo(0));
    QCOMPARE(d.packetsApplied(), 0u);

    d.advanceTo(999);
    QCOMPARE(d.packetsApplied(), CdgDecoder::packetsDueAt(999));
    QCOMPARE(d.pixel(6, 12), 0);

    d.advanceTo(1004);  // packet 300 is due once 1000 ms has passed
    QCOMPARE(d.pixel(6, 12), 6);
    QCOMPARE(d.pixel(0, 0), 0);

    QVERIFY(!d.advanceTo(1004));  // same position: nothing new
    d.advanceTo(1999);
    QCOMPARE(d.pixel(0, 0), 0);
    d.advanceTo(2004);
    QCOMPARE(d.pixel(0, 0), 3);
    QCOMPARE(d.pixel(6, 12), 3);
}

void TestCdgDecoder::advanceIsClampedToStreamLength()
{
    CdgDecoder d;
    d.setData(stream(30, {}));
    QCOMPARE(d.durationMs(), 100);
    d.advanceTo(60'000);
    QCOMPARE(d.packetsApplied(), 30u);
}

void TestCdgDecoder::resetClearsStateButKeepsStream()
{
    CdgDecoder d;
    d.setData(stream(4, {
        {0, loadColors(false, {0x000, 0xFFF, 0, 0, 0, 0, 0, 0})},
        {1, memoryPreset(1)},
        {2, scroll(false, 0, 0, 3, 0, 5)},
    }));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(0, 0), 1);
    QCOMPARE(d.paletteColor(1), 0xFFFFFFFFu);
    QCOMPARE(d.horizontalOffset(), 3);
    const auto revision = d.revision();

    d.reset();
    QCOMPARE(d.packetsApplied(), 0u);
    QCOMPARE(d.packetCount(), 4u);
    QCOMPARE(d.pixel(0, 0), 0);
    QCOMPARE(d.paletteColor(1), 0xFF000000u);
    QCOMPARE(d.horizontalOffset(), 0);
    QCOMPARE(d.verticalOffset(), 0);
    QCOMPARE(d.skippedInstructions(), 0u);
    QVERIFY(d.revision() != revision);  // display must refresh after reset
}

void TestCdgDecoder::replayAfterResetMatchesFreshDecode()
{
    const auto data = stream(1200, {
        {0, loadColors(false, {0x000, 0xF00, 0x0F0, 0x00F, 0, 0, 0, 0})},
        {1, memoryPreset(0)},
        {at(500), solidTile(1, 2, 3)},
        {at(1500), tile(2, 3, 4, 5, {0x2A, 0x15, 0x2A, 0x15, 0x2A, 0x15, 0x2A, 0x15, 0x2A, 0x15, 0x2A, 0x15}, true)},
        {at(2500), scroll(true, 0, 1, 0, 2, 0)},
        {at(3500), borderPreset(3)},
    });

    CdgDecoder played;
    played.setData(data);
    for (int ms = 0; ms <= 3000; ms += 20)
        played.advanceTo(ms);
    played.reset();
    for (int ms = 0; ms <= 4000; ms += 33)
        played.advanceTo(ms);

    CdgDecoder fresh;
    fresh.setData(data);
    fresh.advanceTo(3993);  // last position reached above

    QCOMPARE(played.packetsApplied(), fresh.packetsApplied());
    QVERIFY(screenEquals(played, fresh));
}

void TestCdgDecoder::rewindReplaysFromStart()
{
    const auto data = stream(900, {{at(500), solidTile(4, 0, 0)}, {at(2000), memoryPreset(9)}});
    CdgDecoder d;
    d.setData(data);
    d.advanceTo(2500);
    QCOMPARE(d.pixel(0, 0), 9);

    QVERIFY(d.advanceTo(1000));  // going backwards reports a change
    QCOMPARE(d.packetsApplied(), CdgDecoder::packetsDueAt(1000));
    QCOMPARE(d.pixel(0, 0), 4);
    QCOMPARE(d.pixel(100, 100), 0);

    d.advanceTo(0);
    QCOMPARE(d.pixel(0, 0), 0);
}

void TestCdgDecoder::setDataReplacesStreamAndResets()
{
    CdgDecoder d;
    d.setData(stream(1, {{0, memoryPreset(5)}}));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(0, 0), 5);

    d.setData(stream(10, {}));
    QCOMPARE(d.packetCount(), 10u);
    QCOMPARE(d.packetsApplied(), 0u);
    QCOMPARE(d.pixel(0, 0), 0);

    d.clear();
    QCOMPARE(d.packetCount(), 0u);
}

void TestCdgDecoder::truncatedStreamIgnoresPartialPacket()
{
    auto data = stream(2, {{0, memoryPreset(1)}, {1, memoryPreset(2)}});
    data.resize(data.size() - 5);  // second packet incomplete
    CdgDecoder d;
    d.setData(data);
    QCOMPARE(d.packetCount(), 1u);
    QCOMPARE(d.trailingBytes(), 19u);
    d.advanceTo(10'000);
    QCOMPARE(d.pixel(0, 0), 1);
}

void TestCdgDecoder::emptyAndTinyStreamsAreHarmless()
{
    CdgDecoder d;
    d.setData({});
    QVERIFY(!d.advanceTo(5000));
    d.setData(std::vector<std::uint8_t>(10, 0x09));
    QCOMPARE(d.packetCount(), 0u);
    QVERIFY(!d.advanceTo(5000));
    QVERIFY(!d.advanceTo(-100));
}

void TestCdgDecoder::randomDataDoesNotCrash()
{
    std::vector<std::uint8_t> data(24 * 5000 + 7);
    std::uint32_t seed = 12345;
    for (auto& b : data) {
        seed = seed * 1103515245u + 12345u;
        b = static_cast<std::uint8_t>(seed >> 16);
    }
    // Make many packets graphics packets so every instruction path is hit.
    for (std::size_t i = 0; i + 24 <= data.size(); i += 48)
        data[i] = 0x09;
    CdgDecoder d;
    d.setData(data);
    d.advanceTo(100'000);
    QCOMPARE(d.packetsApplied(), 5000u);
    std::vector<std::uint32_t> image(CdgDecoder::kWidth * CdgDecoder::kHeight);
    d.renderArgb32(image.data(), CdgDecoder::kWidth);
}

void TestCdgDecoder::scrollPresetShiftsAndFills()
{
    CdgDecoder d;
    // Tile at column 1, row 1, then scroll right by one tile and down by one tile.
    d.setData(stream(2, {{0, solidTile(5, 1, 1)}, {1, scroll(false, 7, 1, 0, 1, 0)}}));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(12, 24), 5);   // moved to column 2, row 2
    QCOMPARE(d.pixel(6, 12), 0);    // old location cleared (shifted in from background)
    QCOMPARE(d.pixel(0, 100), 7);   // vacated left strip filled
    QCOMPARE(d.pixel(100, 0), 7);   // vacated top strip filled

    CdgDecoder left;
    left.setData(stream(2, {{0, solidTile(5, 1, 1)}, {1, scroll(false, 7, 2, 0, 2, 0)}}));
    left.advanceTo(1000);
    QCOMPARE(left.pixel(0, 0), 5);      // moved to column 0, row 0
    QCOMPARE(left.pixel(299, 100), 7);  // vacated right strip filled
    QCOMPARE(left.pixel(100, 215), 7);  // vacated bottom strip filled
}

void TestCdgDecoder::scrollCopyWraps()
{
    CdgDecoder d;
    d.setData(stream(2, {{0, solidTile(5, 0, 49)}, {1, scroll(true, 7, 1, 0, 0, 0)}}));
    d.advanceTo(1000);
    QCOMPARE(d.pixel(0, 0), 5);      // right-most column wrapped to the left
    QCOMPARE(d.pixel(294, 0), 0);
    QCOMPARE(d.pixel(100, 100), 0);  // no fill colour in copy mode
}

void TestCdgDecoder::scrollOffsetsAffectRendering()
{
    CdgDecoder d;
    d.setData(stream(3, {
        {0, loadColors(false, {0x000, 0xFFF, 0, 0, 0, 0, 0, 0})},
        {1, solidTile(1, 1, 1)},             // pixels (6..11, 12..23)
        {2, scroll(false, 0, 0, 2, 0, 4)},   // offsets only, no movement
    }));
    d.advanceTo(1000);
    QCOMPARE(d.horizontalOffset(), 2);
    QCOMPARE(d.verticalOffset(), 4);
    QCOMPARE(d.pixel(6, 12), 1);  // memory is not moved by offsets

    std::vector<std::uint32_t> image(CdgDecoder::kWidth * CdgDecoder::kHeight);
    d.renderArgb32(image.data(), CdgDecoder::kWidth);
    auto shown = [&](int x, int y) { return image[y * CdgDecoder::kWidth + x]; };
    QCOMPARE(shown(6, 12), 0xFFFFFFFFu);   // shows memory (8, 16)
    QCOMPARE(shown(9, 19), 0xFFFFFFFFu);   // shows memory (11, 23)
    QCOMPARE(shown(10, 12), 0xFF000000u);  // shows memory (12, 16): outside tile
}

void TestCdgDecoder::renderUsesPalette()
{
    CdgDecoder d;
    d.setData(stream(2, {{0, loadColors(false, {0x000, 0x000, 0x000, 0xF80, 0, 0, 0, 0})}, {1, memoryPreset(3)}}));
    d.advanceTo(1000);
    constexpr int stride = CdgDecoder::kWidth + 4;  // padded rows must be respected
    std::vector<std::uint32_t> image(stride * CdgDecoder::kHeight, 0xDEADBEEF);
    d.renderArgb32(image.data(), stride);
    QCOMPARE(image[0], 0xFFFF8800u);
    QCOMPARE(image[stride * 215 + 299], 0xFFFF8800u);
    QCOMPARE(image[CdgDecoder::kWidth], 0xDEADBEEFu);  // padding untouched
}

QTEST_APPLESS_MAIN(TestCdgDecoder)
#include "tst_cdgdecoder.moc"
