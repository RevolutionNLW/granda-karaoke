#include "music/MusicalKey.h"

#include <array>
#include <cctype>

namespace music {

namespace {

// Majors take the spelling with the fewest accidentals (Db, not C#; Ab, not
// G#), F# as it is usually written in song books. Minors likewise: C#m, F#m,
// G#m, but Ebm and Bbm.
constexpr std::array<const char*, 12> kMajorNames = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
constexpr std::array<const char*, 12> kMinorNames = {
    "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm"};

int wrap(int pitchClass)
{
    const int value = pitchClass % 12;
    return value < 0 ? value + 12 : value;
}

} // namespace

std::optional<MusicalKey> MusicalKey::fromIndex(int index)
{
    if (index < 0 || index > 23)
        return std::nullopt;
    return MusicalKey{index % 12, index >= 12};
}

MusicalKey MusicalKey::transposed(int semitones) const
{
    return MusicalKey{wrap(tonic + semitones), minor};
}

std::string MusicalKey::name() const
{
    const int pc = wrap(tonic);
    return minor ? kMinorNames[static_cast<std::size_t>(pc)]
                 : kMajorNames[static_cast<std::size_t>(pc)];
}

std::optional<MusicalKey> MusicalKey::parse(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    if (text.empty())
        return std::nullopt;

    static constexpr std::array<int, 7> kLetters = {9, 11, 0, 2, 4, 5, 7};  // A..G
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
    if (letter < 'A' || letter > 'G')
        return std::nullopt;
    int tonic = kLetters[static_cast<std::size_t>(letter - 'A')];
    text.remove_prefix(1);
    if (!text.empty() && (text.front() == '#' || text.front() == 'b')) {
        tonic += text.front() == '#' ? 1 : -1;
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);

    bool minor = false;
    if (!text.empty()) {
        std::string rest;
        for (const char c : text)
            rest.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (rest == "m" || rest == "min" || rest == "minor")
            minor = true;
        else if (rest != "maj" && rest != "major")
            return std::nullopt;
    }
    return MusicalKey{wrap(tonic), minor};
}

} // namespace music
