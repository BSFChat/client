#include "voice/video/MediaCodecAnnexB.h"

#include <vector>

namespace annexb {
namespace {

static const char kStartCode[4] = {0, 0, 0, 1};

// Walk the NAL units of an Annex-B buffer, calling `fn(type, begin, end)`
// for each, where [begin, end) is the NAL payload WITHOUT its start code.
//
// Handles both 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes:
// a 4-byte code is a 3-byte code with an extra leading zero, and that
// zero sits at the tail of the previous NAL's range, so it is trimmed
// there. Same shape as MacVTDecoder's splitAnnexB, which is where this
// came from.
template <typename Fn>
void forEachNal(const QByteArray& au, Fn fn) {
    const auto* p = reinterpret_cast<const uint8_t*>(au.constData());
    const int n = au.size();
    // Parallel arrays: where each start code begins, and where the NAL
    // after it begins.
    std::vector<int> codeStart, nalStart;
    for (int i = 0; i + 2 < n;) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            codeStart.push_back(i);
            nalStart.push_back(i + 3);
            i += 3;
        } else {
            ++i;
        }
    }
    for (size_t k = 0; k < nalStart.size(); ++k) {
        int end = (k + 1 < nalStart.size()) ? codeStart[k + 1] : n;
        if (k + 1 < nalStart.size() && end > nalStart[k] && p[end - 1] == 0)
            --end;
        if (end > nalStart[k]) fn(uint8_t(p[nalStart[k]] & 0x1F), nalStart[k], end);
    }
}

} // namespace

Summary scan(const QByteArray& au) {
    Summary s;
    forEachNal(au, [&s](uint8_t type, int, int) {
        switch (type) {
        case 1: s.hasSlice = true; break;
        case 5: s.hasSlice = true; s.hasIdr = true; break;
        case 7: s.hasSps = true; break;
        case 8: s.hasPps = true; break;
        default: break;
        }
    });
    return s;
}

bool parameterSets(const QByteArray& au, QByteArray& sps, QByteArray& pps) {
    QByteArray foundSps, foundPps;
    forEachNal(au, [&](uint8_t type, int begin, int end) {
        if (type != 7 && type != 8) return;
        QByteArray nal(kStartCode, 4);
        nal.append(au.constData() + begin, end - begin);
        // Last one wins: see the header on mid-stream resolution change.
        if (type == 7) foundSps = nal;
        else           foundPps = nal;
    });
    if (foundSps.isEmpty() || foundPps.isEmpty()) return false;
    sps = foundSps;
    pps = foundPps;
    return true;
}

} // namespace annexb
