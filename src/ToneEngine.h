#pragma once

#include "Document.h"

#include <QImage>

namespace pp {

/// Conservative tone suggestion. Analyses at most about one megapixel of the
/// already oriented, linear image. No mask, no face detection, no local glow.
class ToneEngine
{
public:
    static PhotoTone suggest(const QImage &linearSample);
};

} // namespace pp
