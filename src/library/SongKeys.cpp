#include "library/SongKeys.h"

QString songKeyName(int keyIndex)
{
    return transposedKeyName(keyIndex, 0);
}

QString transposedKeyName(int keyIndex, int semitones)
{
    const std::optional<music::MusicalKey> key = music::MusicalKey::fromIndex(keyIndex);
    if (!key)
        return {};
    return QString::fromStdString(key->transposed(semitones).name());
}
