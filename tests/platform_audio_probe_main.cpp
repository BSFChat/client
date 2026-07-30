#include "voice/PlatformAudioProbe.h"
#include <QCoreApplication>
#include <QTextStream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    const auto r = probePlatformAudio();
    out << "constructed=" << r.constructed << " error=" << r.error << "\n";
    out << "livekit recordingCount=" << r.recordingCount
        << " playoutCount=" << r.playoutCount << "\n";
    out << "-- livekit recording devices (name | id) --\n";
    for (int i = 0; i < r.recordingNames.size(); ++i)
        out << "   [" << i << "] " << r.recordingNames[i] << "  |  "
            << (i < r.recordingIds.size() ? r.recordingIds[i] : QString()) << "\n";
    out << "-- livekit playout devices --\n";
    for (const auto& n : r.playoutNames) out << "   " << n << "\n";
    out << "-- Qt audioInputs().description() --\n";
    for (const auto& n : r.qtInputDescriptions) out << "   " << n << "\n";
    out << "-- Qt audioOutputs().description() --\n";
    for (const auto& n : r.qtOutputDescriptions) out << "   " << n << "\n";
    out << "exactInputNameMatches=" << r.exactInputNameMatches
        << " of " << r.recordingNames.size() << "\n";
    return 0;
}
