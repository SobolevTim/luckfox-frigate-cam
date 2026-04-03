/*
 * camera_mpi.cc — VI capture + VENC hardware encoder helpers + RGA resize
 *
 * Uses NV12 (YUV420SP) throughout — no CPU colour-space conversion.
 */

#include "camera_mpi.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "im2d.h"
#include "rga.h"

/* ── Timing ──────────────────────────────────────────────────────────────── */

RK_U64 get_now_us(void) {
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (RK_U64)ts.tv_sec * 1000000ULL + (RK_U64)ts.tv_nsec / 1000ULL;
}

/* ── VI device ───────────────────────────────────────────────────────────── */

int vi_dev_init(void) {
    const int devId  = VI_DEV_ID;
    const int pipeId = devId;

    VI_DEV_ATTR_S    stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    memset(&stDevAttr,   0, sizeof(stDevAttr));
    memset(&stBindPipe,  0, sizeof(stBindPipe));

    int ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
        if (ret != RK_SUCCESS) {
            printf("[VI] SetDevAttr failed: 0x%x\n", ret);
            return -1;
        }
    }

    ret = RK_MPI_VI_GetDevIsEnable(devId);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(devId);
        if (ret != RK_SUCCESS) {
            printf("[VI] EnableDev failed: 0x%x\n", ret);
            return -1;
        }
        stBindPipe.u32Num    = 1;
        stBindPipe.PipeId[0] = pipeId;
        ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
        if (ret != RK_SUCCESS) {
            printf("[VI] SetDevBindPipe failed: 0x%x\n", ret);
            return -1;
        }
    }
    printf("[VI] device %d ready\n", devId);
    return 0;
}

int vi_chn_init(int dev_id, int chn_id, int width, int height) {
    VI_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));

    /* 2 buffers with MMAP — DMABUF is not supported on RV1106 ISP mainpath */
    attr.stIspOpt.u32BufCount   = 2;
    attr.stIspOpt.enMemoryType  = VI_V4L2_MEMORY_TYPE_MMAP;
    attr.stSize.u32Width        = (RK_U32)width;
    attr.stSize.u32Height       = (RK_U32)height;
    attr.enPixelFormat          = RK_FMT_YUV420SP; /* NV12 — native ISP output */
    attr.enCompressMode         = COMPRESS_MODE_NONE;
    /* u32Depth must be <= u32BufCount when channel is not bound to another device */
    attr.u32Depth               = 2;

    int ret = RK_MPI_VI_SetChnAttr(dev_id, chn_id, &attr);
    ret    |= RK_MPI_VI_EnableChn(dev_id, chn_id);
    if (ret) {
        printf("[VI] channel init failed: %d\n", ret);
        return -1;
    }
    printf("[VI] channel %d/%d → %dx%d NV12\n", dev_id, chn_id, width, height);
    return 0;
}

void vi_deinit(int dev_id, int chn_id) {
    RK_MPI_VI_DisableChn(dev_id, chn_id);
    RK_MPI_VI_DisableDev(dev_id);
    printf("[VI] stopped\n");
}

/* ── VENC ────────────────────────────────────────────────────────────────── */

int venc_init(int chn_id, int width, int height,
              RK_CODEC_ID_E type, int bitrate_kbps, int fps, int gop) {
    VENC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));

    /* Soft safety bounds — actual range validation is in isp_control layer */
    if (bitrate_kbps < 1) bitrate_kbps = 1;
    if (fps < 1) fps = 1;
    if (gop < 1) gop = 1;

    /* Rate-control: CBR for stable Frigate buffering */
    if (type == RK_VIDEO_ID_AVC) {
        attr.stRcAttr.enRcMode                        = VENC_RC_MODE_H264CBR;
        attr.stRcAttr.stH264Cbr.u32BitRate            = (RK_U32)bitrate_kbps;
        attr.stRcAttr.stH264Cbr.u32Gop                = (RK_U32)gop;
        attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum    = (RK_U32)fps;
        attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen    = 1;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum   = (RK_U32)fps;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen   = 1;
    } else if (type == RK_VIDEO_ID_HEVC) {
        attr.stRcAttr.enRcMode                        = VENC_RC_MODE_H265CBR;
        attr.stRcAttr.stH265Cbr.u32BitRate            = (RK_U32)bitrate_kbps;
        attr.stRcAttr.stH265Cbr.u32Gop                = (RK_U32)gop;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum    = (RK_U32)fps;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen    = 1;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum   = (RK_U32)fps;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen   = 1;
    } else {
        printf("[VENC] unsupported codec type %d\n", type);
        return -1;
    }

    /* Encoder attributes */
    attr.stVencAttr.enType          = type;
    attr.stVencAttr.enPixelFormat   = RK_FMT_YUV420SP; /* same as VI — no conversion */
    if (type == RK_VIDEO_ID_AVC)
        attr.stVencAttr.u32Profile  = H264E_PROFILE_HIGH;
    attr.stVencAttr.u32PicWidth     = (RK_U32)width;
    attr.stVencAttr.u32PicHeight    = (RK_U32)height;
    attr.stVencAttr.u32VirWidth     = (RK_U32)width;
    attr.stVencAttr.u32VirHeight    = (RK_U32)height;
    attr.stVencAttr.u32StreamBufCnt = 4;  /* extra output buffers for stability */
    attr.stVencAttr.u32BufSize      = (RK_U32)(width * height * 3 / 2);
    attr.stVencAttr.enMirror        = MIRROR_NONE;

    int ret = RK_MPI_VENC_CreateChn(chn_id, &attr);
    if (ret != RK_SUCCESS) {
        printf("[VENC] CreateChn failed: 0x%x\n", ret);
        return -1;
    }

    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1; /* receive indefinitely */
    RK_MPI_VENC_StartRecvFrame(chn_id, &recv);

    printf("[VENC] channel %d: %dx%d  %d kbps  %dfps  GOP=%d\n",
           chn_id, width, height, bitrate_kbps, fps, gop);
    return 0;
}

int venc_set_bitrate(int chn_id, int bitrate_kbps) {
    VENC_CHN_ATTR_S attr;
    if (bitrate_kbps < 1) bitrate_kbps = 1;
    int ret = RK_MPI_VENC_GetChnAttr(chn_id, &attr);
    if (ret != RK_SUCCESS) return -1;

    if (attr.stRcAttr.enRcMode == VENC_RC_MODE_H264CBR)
        attr.stRcAttr.stH264Cbr.u32BitRate = (RK_U32)bitrate_kbps;
    else if (attr.stRcAttr.enRcMode == VENC_RC_MODE_H265CBR)
        attr.stRcAttr.stH265Cbr.u32BitRate = (RK_U32)bitrate_kbps;
    else
        return -1;

    ret = RK_MPI_VENC_SetChnAttr(chn_id, &attr);
    if (ret == RK_SUCCESS)
        printf("[VENC] ch%d bitrate updated → %d kbps\n", chn_id, bitrate_kbps);
    return (ret == RK_SUCCESS) ? 0 : -1;
}

int venc_set_fps(int chn_id, int fps, int gop) {
    VENC_CHN_ATTR_S attr;
    if (fps < 1) fps = 1;
    if (gop < 1) gop = 1;

    int ret = RK_MPI_VENC_GetChnAttr(chn_id, &attr);
    if (ret != RK_SUCCESS) return -1;

    if (attr.stRcAttr.enRcMode == VENC_RC_MODE_H264CBR) {
        attr.stRcAttr.stH264Cbr.u32Gop              = (RK_U32)gop;
        attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum  = (RK_U32)fps;
        attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen  = 1;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = (RK_U32)fps;
        attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    } else if (attr.stRcAttr.enRcMode == VENC_RC_MODE_H265CBR) {
        attr.stRcAttr.stH265Cbr.u32Gop              = (RK_U32)gop;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum  = (RK_U32)fps;
        attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen  = 1;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = (RK_U32)fps;
        attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
    } else {
        return -1;
    }

    ret = RK_MPI_VENC_SetChnAttr(chn_id, &attr);
    if (ret == RK_SUCCESS)
        printf("[VENC] ch%d fps updated → %d fps, GOP=%d\n", chn_id, fps, gop);
    return (ret == RK_SUCCESS) ? 0 : -1;
}

void venc_deinit(int chn_id) {
    RK_MPI_VENC_StopRecvFrame(chn_id);
    RK_MPI_VENC_DestroyChn(chn_id);
    printf("[VENC] channel %d stopped\n", chn_id);
}

/* ── RGA hardware copy (DMA, zero CPU load) ──────────────────────────────── */

int rga_copy_nv12(void *src_vaddr, void *dst_vaddr, int width, int height) {
    rga_buffer_t src = wrapbuffer_virtualaddr(src_vaddr,
                                              width, height,
                                              RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_vaddr,
                                              width, height,
                                              RK_FORMAT_YCbCr_420_SP);

    IM_STATUS status = imcopy_t(src, dst, 1);
    if (status != IM_STATUS_SUCCESS) {
        printf("[RGA] copy %dx%d failed: %s\n",
               width, height, imStrError(status));
        return -1;
    }
    return 0;
}

int rga_copy_nv12_transform(void *src_vaddr, void *dst_vaddr,
                             int width, int height, int mirror, int flip) {
    if (!mirror && !flip)
        return rga_copy_nv12(src_vaddr, dst_vaddr, width, height);

    rga_buffer_t src = wrapbuffer_virtualaddr(src_vaddr,
                                              width, height,
                                              RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_vaddr,
                                              width, height,
                                              RK_FORMAT_YCbCr_420_SP);

    IM_STATUS status;
    if (mirror && flip) {
        /* H+V flip is equivalent to a 180° rotation — single RGA pass */
        status = imrotate_t(src, dst, IM_HAL_TRANSFORM_ROT_180, 1);
    } else if (mirror) {
        status = imflip_t(src, dst, IM_HAL_TRANSFORM_FLIP_H, 1);
    } else {
        status = imflip_t(src, dst, IM_HAL_TRANSFORM_FLIP_V, 1);
    }

    if (status != IM_STATUS_SUCCESS) {
        printf("[RGA] transform %dx%d (mirror=%d flip=%d) failed: %s\n",
               width, height, mirror, flip, imStrError(status));
        return -1;
    }
    return 0;
}

/* ── RGA hardware resize ─────────────────────────────────────────────────── */

int rga_resize_nv12(void *src_vaddr, int src_w, int src_h,
                    void *dst_vaddr, int dst_w, int dst_h) {
    rga_buffer_t src = wrapbuffer_virtualaddr(src_vaddr,
                                              src_w, src_h,
                                              RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_vaddr,
                                              dst_w, dst_h,
                                              RK_FORMAT_YCbCr_420_SP);

    IM_STATUS status = imresize_t(src, dst, 0, 0, INTER_LINEAR, 1);
    if (status != IM_STATUS_SUCCESS) {
        printf("[RGA] resize %dx%d→%dx%d failed: %s\n",
               src_w, src_h, dst_w, dst_h, imStrError(status));
        return -1;
    }
    return 0;
}
