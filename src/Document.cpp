#include "Document.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>

namespace pp {

namespace {

void writeDouble(QDataStream &stream, double value)
{
    // Fixed textual precision keeps the digest stable across platforms while
    // still covering the 0.01 mm model precision.
    stream << QString::number(value, 'f', 6);
}

} // namespace

QByteArray ColorRecipe::digest() const
{
    QByteArray raw;
    QDataStream stream(&raw, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << qint32(version);
    stream << qint8(autoEnhance ? 1 : 0);
    writeDouble(stream, tone.exposureEv);
    writeDouble(stream, tone.shadowLift);
    writeDouble(stream, tone.highlightCompress);
    writeDouble(stream, paperStrength);
    stream << qint8(grayscale ? 1 : 0);
    return QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
}

const Frame *findFrame(const DocumentSnapshot &snapshot, const QString &frameId, int *pageIndex)
{
    for (int i = 0; i < snapshot.pages.size(); ++i) {
        const Page &page = snapshot.pages.at(i);
        for (const Frame &frame : page.frames) {
            if (frame.id == frameId) {
                if (pageIndex)
                    *pageIndex = i;
                return &frame;
            }
        }
    }
    return nullptr;
}

Frame *findFrame(DocumentSnapshot &snapshot, const QString &frameId, int *pageIndex)
{
    for (int i = 0; i < snapshot.pages.size(); ++i) {
        Page &page = snapshot.pages[i];
        for (Frame &frame : page.frames) {
            if (frame.id == frameId) {
                if (pageIndex)
                    *pageIndex = i;
                return &frame;
            }
        }
    }
    return nullptr;
}

const Page *findPage(const DocumentSnapshot &snapshot, const QString &pageId, int *pageIndex)
{
    for (int i = 0; i < snapshot.pages.size(); ++i) {
        if (snapshot.pages.at(i).id == pageId) {
            if (pageIndex)
                *pageIndex = i;
            return &snapshot.pages.at(i);
        }
    }
    return nullptr;
}

int pageIndexOf(const DocumentSnapshot &snapshot, const QString &pageId)
{
    int index = -1;
    findPage(snapshot, pageId, &index);
    return index;
}

PhotoTone effectiveTone(const Frame &frame)
{
    PhotoTone tone = frame.autoTone ? frame.tone : PhotoTone{};
    tone.exposureEv += frame.brightness;
    return tone;
}

QSizeF pageSizeMm(const Page &page)
{
    return page.landscape ? QSizeF(page.paper.physicalHeightMm, page.paper.physicalWidthMm)
                          : QSizeF(page.paper.physicalWidthMm, page.paper.physicalHeightMm);
}

} // namespace pp
