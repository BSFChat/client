#pragma once

#include <QString>
#include <functional>

// AVFoundation-backed camera permission helper. Used on macOS to
// replace Qt's (Homebrew-Qt-missing) QCameraPermission plugin. The
// TCC prompt appears on the first call to request() when the
// current status is "undetermined".
namespace mac_camera_permission {
QString status();
void request(std::function<void(bool granted)> callback);
}
