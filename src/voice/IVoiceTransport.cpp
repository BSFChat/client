// IVoiceTransport is a pure interface — every method is virtual and the
// class has no state, so there is nothing to implement here.
//
// This translation unit exists anyway, and must not be deleted, for one
// mechanical reason: CMake's AUTOMOC only scans a header for Q_OBJECT
// when that header shares a basename with a source file in the target,
// or is listed in the target's SOURCES directly. IVoiceTransport.h
// declares Q_OBJECT and all of the transport signals, so without a
// matching .cpp its moc is never generated and every `emit` from
// VoiceEngine.cpp fails to link with undefined references to
// IVoiceTransport::staticMetaObject and the signal emitters.
//
// Being included by VoiceEngine.h is not enough — AUTOMOC does not
// follow includes.

#include "voice/IVoiceTransport.h"
