#ifndef AVCODEC_DRMPRIME_H
#define AVCODEC_DRMPRIME_H

#include <stdint.h>

#define AV_FF_DRMPRIME_NUM_PLANES	4	// maximum number of planes

typedef struct av_drmprime {

    int strides[AV_FF_DRMPRIME_NUM_PLANES];
    int offsets[AV_FF_DRMPRIME_NUM_PLANES];
    int fd[AV_FF_DRMPRIME_NUM_PLANES];
    uint32_t format;

} av_drmprime;

#endif // AVCODEC_DRMPRIME_H
