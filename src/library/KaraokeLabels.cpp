#include "library/KaraokeLabels.h"

#include <QHash>
#include <QSet>
#include <QStringList>

namespace {

// Disc prefixes (letters of a disc ID) that belong to one label only. A
// prefix used by several producers (for example "PS" or "LG") is left out.
const QHash<QString, QString>& prefixLabels()
{
    static const QHash<QString, QString> labels = [] {
        QHash<QString, QString> map;
        auto add = [&map](const QString& label, const QStringList& prefixes) {
            for (const QString& prefix : prefixes)
                map.insert(prefix, label);
        };
        add(QStringLiteral("Sunfly"), {QStringLiteral("SF"), QStringLiteral("SFMW"), QStringLiteral("SFG"),
                                       QStringLiteral("SFGD"), QStringLiteral("SUNFLY")});
        add(QStringLiteral("Legends"), {QStringLiteral("LEG")});
        add(QStringLiteral("Zoom"), {QStringLiteral("ZKH"), QStringLiteral("ZKL"), QStringLiteral("ZKP"),
                                     QStringLiteral("ZPA"), QStringLiteral("ZMP"), QStringLiteral("ZMPA"),
                                     QStringLiteral("ZML"), QStringLiteral("ZMH"), QStringLiteral("ZMJ"),
                                     QStringLiteral("ZMJB"), QStringLiteral("ZMGY"), QStringLiteral("ZMG"),
                                     QStringLiteral("ZGY"), QStringLiteral("ZOA"), QStringLiteral("ZOOM"),
                                     QStringLiteral("ZOOMJB"), QStringLiteral("ZOOMPA")});
        add(QStringLiteral("DK Karaoke"), {QStringLiteral("DK"), QStringLiteral("DKM")});
        add(QStringLiteral("Monster Hits"), {QStringLiteral("MH")});
        add(QStringLiteral("Mr Entertainer"), {QStringLiteral("MRH"), QStringLiteral("MRE"),
                                               QStringLiteral("MREH"), QStringLiteral("MRHB")});
        add(QStringLiteral("Sweet Georgia Brown"), {QStringLiteral("SGB"), QStringLiteral("SGBCRP")});
        add(QStringLiteral("Sound Choice"), {QStringLiteral("SC")});
        add(QStringLiteral("Essential Karaoke"), {QStringLiteral("ET"), QStringLiteral("EK"),
                                                  QStringLiteral("EKI"), QStringLiteral("EKX"),
                                                  QStringLiteral("EX")});
        add(QStringLiteral("Easy Karaoke"), {QStringLiteral("EZ"), QStringLiteral("EZH"), QStringLiteral("EZC"),
                                             QStringLiteral("EZA"), QStringLiteral("EZHXM")});
        add(QStringLiteral("Priddis"), {QStringLiteral("PM"), QStringLiteral("PMES")});
        add(QStringLiteral("Chartbuster"), {QStringLiteral("CB"), QStringLiteral("CBE")});
        add(QStringLiteral("Pioneer"), {QStringLiteral("PI")});
        add(QStringLiteral("Top Hits Monthly"), {QStringLiteral("THM"), QStringLiteral("THMR"),
                                                 QStringLiteral("THMC"), QStringLiteral("THMH"),
                                                 QStringLiteral("THMP")});
        return map;
    }();
    return labels;
}

// Series that are certain from the prefix alone.
QString seriesFor(const QString& prefix)
{
    if (prefix == QLatin1String("SFMW"))
        return QStringLiteral("Most Wanted");
    if (prefix == QLatin1String("SFG") || prefix == QLatin1String("SFGD"))
        return QStringLiteral("Gold");
    return {};
}

// Top-level folders named after one label. "Essential" holds both Essential
// and Easy Karaoke discs, so it only confirms either of those.
const QHash<QString, QStringList>& folderLabels()
{
    static const QHash<QString, QStringList> folders = {
        {QStringLiteral("sunfly"), {QStringLiteral("Sunfly")}},
        {QStringLiteral("legends"), {QStringLiteral("Legends")}},
        {QStringLiteral("zooms"), {QStringLiteral("Zoom")}},
        {QStringLiteral("zoom"), {QStringLiteral("Zoom")}},
        {QStringLiteral("dk karaoke"), {QStringLiteral("DK Karaoke")}},
        {QStringLiteral("monster hits"), {QStringLiteral("Monster Hits")}},
        {QStringLiteral("mr entertainer"), {QStringLiteral("Mr Entertainer")}},
        {QStringLiteral("sweet georgia brown"), {QStringLiteral("Sweet Georgia Brown")}},
        {QStringLiteral("sc"), {QStringLiteral("Sound Choice")}},
        {QStringLiteral("pocketsongs"), {QStringLiteral("Pocket Songs")}},
        {QStringLiteral("pocket songs"), {QStringLiteral("Pocket Songs")}},
        {QStringLiteral("priddis"), {QStringLiteral("Priddis")}},
        {QStringLiteral("top hits monthly"), {QStringLiteral("Top Hits Monthly")}},
        {QStringLiteral("para top monthly hits"), {QStringLiteral("Top Hits Monthly")}},
        {QStringLiteral("bassline"), {QStringLiteral("Bassline")}},
        {QStringLiteral("pioneer"), {QStringLiteral("Pioneer")}},
        {QStringLiteral("cb"), {QStringLiteral("Chartbuster")}},
        {QStringLiteral("music maestro"), {QStringLiteral("Music Maestro")}},
        {QStringLiteral("essential"), {QStringLiteral("Essential Karaoke"), QStringLiteral("Easy Karaoke")}},
    };
    return folders;
}

} // namespace

KaraokeLabel identifyKaraokeLabel(const QString& discPrefix, const QString& relDir)
{
    const QString prefix = discPrefix.toUpper();
    const QString top = relDir.section(QLatin1Char('/'), 0, 0).simplified().toCaseFolded();
    const QStringList fromFolder = folderLabels().value(top);
    // Generic words are not label prefixes ("Vol 3", "CD 2").
    static const QSet<QString> generic = {QStringLiteral("VOL"), QStringLiteral("CD"),
                                          QStringLiteral("DISC"), QStringLiteral("TRACK")};
    const bool noPrefix = prefix.isEmpty() || generic.contains(prefix);
    KaraokeLabel result;
    if (!noPrefix) {
        const auto known = prefixLabels().constFind(prefix);
        if (known == prefixLabels().cend())
            return result;  // an unknown prefix: say nothing
        if (!fromFolder.isEmpty() && !fromFolder.contains(*known))
            return result;  // the folder names another label: uncertain
        result.label = *known;
        result.series = seriesFor(prefix);
        result.source = QStringLiteral("disc_prefix");
        return result;
    }
    if (fromFolder.size() == 1) {
        result.label = fromFolder.first();
        result.source = QStringLiteral("folder");
        // Sunfly keeps its series in folders ("sunfly most wanted/820").
        const QString second = relDir.section(QLatin1Char('/'), 1, 1).toCaseFolded();
        if (result.label == QLatin1String("Sunfly") && second.contains(QLatin1String("most wanted")))
            result.series = QStringLiteral("Most Wanted");
        else if (result.label == QLatin1String("Sunfly") && second.contains(QLatin1String("gold")))
            result.series = QStringLiteral("Gold");
    }
    return result;
}
