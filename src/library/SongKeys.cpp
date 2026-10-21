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

QString songKeyDisplay(int keyIndex, int semitones)
{
    const QString original = songKeyName(keyIndex);
    if (original.isEmpty() || semitones == 0)
        return original;
    return original + QStringLiteral(" \u2192 ") + transposedKeyName(keyIndex, semitones);
}
