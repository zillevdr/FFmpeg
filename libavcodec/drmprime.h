#ifndef AVCODEC_DRMPRIME_H
#define AVCODEC_DRMPRIME_H

#include <stdint.h>

#define AV_FF_DRMPRIME_NUM_PLANES	4	// maximum number of planes

typedef struct av_drmprime {

    int strides[AV_FF_DRMPRIME_NUM_PLANES]; // stride in byte of the according plane
    int offsets[AV_FF_DRMPRIME_NUM_PLANES]; // offset from start in byte of the according plane
    int fds[AV_FF_DRMPRIME_NUM_PLANES];     // file descriptor for the plane, (0) if unused.
    uint32_t format;                        // FOURC_CC drm format for the image

} av_drmprime;

#endif // AVCODEC_DRMPRIME_H
