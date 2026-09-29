// A few timings that separate plain computing from SQLite work, to compare
// machines (the metadata resolver runs far slower on the Windows runner):
//
//   fks-bench <empty folder>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <cstdio>

namespace {

void report(const char* what, qint64 ms)
{
    std::printf("%-58s %7lld ms\n", what, static_cast<long long>(ms));
    std::fflush(stdout);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() != 2 || !QDir().mkpath(args.at(1))) {
        std::fprintf(stderr, "Usage: fks-bench <empty folder>\n");
        return 2;
    }
    constexpr int kRows = 20000;
    QElapsedTimer timer;

    timer.start();
    QHash<QString, int> words;
    qint64 letters = 0;
    for (int i = 0; i < kRows; ++i) {
        const QString text = QStringLiteral("Presley, Elvis - Suspicious Minds (Karaoke Version) %1").arg(i);
        const QString folded = text.toCaseFolded().normalized(QString::NormalizationForm_KD);
        for (const QString& word : folded.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            ++words[word];
        letters += folded.size();
    }
    report("strings: fold, normalise, split, count (20k titles)", timer.elapsed());

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("bench"));
    db.setDatabaseName(QDir(args.at(1)).filePath(QStringLiteral("bench.sqlite")));
    if (!db.open()) {
        std::fprintf(stderr, "open: %s\n", qPrintable(db.lastError().text()));
        return 1;
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("CREATE TABLE songs(id INTEGER PRIMARY KEY, title TEXT, artist TEXT)"));

    timer.restart();
    db.transaction();
    q.prepare(QStringLiteral("INSERT INTO songs(title, artist) VALUES(?, ?)"));
    for (int i = 0; i < kRows; ++i) {
        q.addBindValue(QStringLiteral("Title %1").arg(i));
        q.addBindValue(QStringLiteral("Artist %1").arg(i % 500));
        q.exec();
    }
    db.commit();
    report("sqlite: 20k inserts in one transaction", timer.elapsed());

    timer.restart();
    q.prepare(QStringLiteral("SELECT title FROM songs WHERE id=?"));
    for (int i = 1; i <= kRows; ++i) {
        q.addBindValue(i);
        q.exec();
        q.next();
    }
    report("sqlite: 20k single-row selects, each on its own", timer.elapsed());

    timer.restart();
    db.transaction();
    for (int i = 1; i <= kRows; ++i) {
        q.addBindValue(i);
        q.exec();
        q.next();
    }
    db.commit();
    report("sqlite: 20k single-row selects inside one transaction", timer.elapsed());

    timer.restart();
    q.prepare(QStringLiteral("UPDATE songs SET title=title||'' WHERE id=?"));
    for (int i = 1; i <= 2000; ++i) {
        q.addBindValue(i);
        q.exec();
    }
    report("sqlite: 2k updates, each its own transaction (synced)", timer.elapsed());

    q.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    timer.restart();
    for (int i = 1; i <= 2000; ++i) {
        q.addBindValue(i);
        q.exec();
    }
    report("sqlite: 2k updates, each its own, synchronous=NORMAL", timer.elapsed());

    timer.restart();
    db.transaction();
    for (int i = 1; i <= kRows; ++i) {
        q.addBindValue(i);
        q.exec();
    }
    db.commit();
    report("sqlite: 20k updates inside one transaction", timer.elapsed());

    q.finish();
    db.close();
    std::printf("(%lld letters, %lld distinct words)\n", static_cast<long long>(letters),
                static_cast<long long>(words.size()));
    return 0;
}
