#pragma once

#include <QList>
#include <QString>
#include <QtTypes>

struct ZipMember {
    QString name;
    quint16 method = 0;
    quint32 crc32 = 0;
    quint64 compressedSize = 0;
    quint64 uncompressedSize = 0;
    quint64 localHeaderOffset = 0;
    bool encrypted = false;
    bool isDirectory = false;
    bool damaged = false;
};

enum class ZipStatus { Ok, NotZip, Truncated, Corrupt, Unreadable };

struct ZipDirectoryResult {
    ZipStatus status = ZipStatus::NotZip;
    QString detail;
    QList<ZipMember> members;
};

ZipDirectoryResult readZipDirectory(const QString& path);
bool zipMethodSupported(quint16 method);
