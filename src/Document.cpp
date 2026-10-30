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
    QString text = QString::number(value, 'f', 6);
    // A negative value that rounds to zero formats as "-0.000000", which is the
    // same number as "0.000000". Normalise the text, not the double: the double
    // check alone misses values such as -1e-9.
    if (text.startsWith(QLatin1Char('-'))) {
        QString digits = text.mid(1);
        digits.remove(QLatin1Char('.'));
        digits.remove(QLatin1Char('0'));
        if (digits.isEmpty())
            text.remove(0, 1);
    }
    stream << text;
}

} // namespace

QByteArray ColorRecipe::digest() const
{
    QByteArray raw;
    QDataStream stream(&raw, QIODevice::WriteOnly);
    // Pinned so a Qt upgrade cannot silently change the stream layout and
    // therefore every cache key and recipe digest. Qt_6_0 is the version the
    // unpinned stream used in 1.0.0, so existing digests stay unchanged.
    stream.setVersion(QDataStream::Qt_6_0);
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

PhotoTone sanitiseTone(const PhotoTone &tone)
{
    PhotoTone sane;
    sane.exposureEv = std::isfinite(tone.exposureEv)
        ? std::clamp(tone.exposureEv, kMinToneExposureEv, kMaxToneExposureEv)
        : 0.0;
    sane.shadowLift = std::isfinite(tone.shadowLift)
        ? std::clamp(tone.shadowLift, 0.0, kMaxToneShadowLift)
        : 0.0;
    sane.highlightCompress = std::isfinite(tone.highlightCompress)
        ? std::clamp(tone.highlightCompress, 0.0, kMaxToneHighlightCompress)
        : 0.0;
    return sane;
}

PhotoTone effectiveTone(const Frame &frame)
{
    PhotoTone tone = frame.autoTone ? frame.tone : PhotoTone{};
    // A project file can name any number, so the frame's own tone is sanitised
    // here, at the boundary where it becomes a recipe: an unclamped exposure
    // would drive the frame to solid black or white in the renderer.
    tone = sanitiseTone(tone);
    // The manual offset is clamped on its own, then added: a project file can
    // name any brightness too, and the sum still has to stay in the range the
    // renderer was promised.
    tone.exposureEv += clampBrightnessEv(frame.brightness);
    tone.exposureEv = std::clamp(tone.exposureEv, kMinToneExposureEv, kMaxToneExposureEv);
    return tone;
}

QSizeF pageSizeMm(const Page &page)
{
    return page.landscape ? QSizeF(page.paper.physicalHeightMm, page.paper.physicalWidthMm)
                          : QSizeF(page.paper.physicalWidthMm, page.paper.physicalHeightMm);
}

} // namespace pp
