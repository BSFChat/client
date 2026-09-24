#pragma once

#include "voice/video/VideoCodec.h"

class QVideoFrame;

namespace FrameConverter {

// Convert a captured QVideoFrame into an encoder-ready I420 frame,
// downscaled so the long edge is ≤ maxLongEdge (0 = no limit) and both
// dimensions are even (4:2:0 requirement). Returns an invalid frame on
// unmappable/unsupported input. Runs on the encode worker thread.
//
// The frame's presentation rotation is APPLIED to the pixels, so the
// returned frame is upright for a receiver that has no orientation
// metadata — which is every receiver, since H.264/H.265 over RTP does
// not carry one here. That means the returned width and height are
// SWAPPED relative to the input for a 90°/270° source (a portrait phone),
// and callers that size an encoder session must take the dimensions from
// the returned frame rather than from the input. VideoSendPipeline
// already does. See VideoFrameOrientation.h for the whole argument,
// including why the mirror flag is not applied.
PlanarFrame toI420(const QVideoFrame& in, int maxLongEdge, qint64 captureTimeUs);

// Identity-matrix 4:4:4 conversion for the true-lossless tier: RGB
// planes mapped G→Y, B→U, R→V with no colorspace transform, so the
// receiver reconstructs the source RGB bit-exactly. (4:2:0 or a real
// YUV matrix would destroy losslessness.) Lands with the AV1 tier.
PlanarFrame toI444Identity(const QVideoFrame& in, qint64 captureTimeUs);

} // namespace FrameConverter
