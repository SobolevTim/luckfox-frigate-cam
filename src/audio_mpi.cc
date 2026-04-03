#include "audio_mpi.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

/* ── Codec hardware bootstrap via amixer ──────────────────────────────────── */

/*
 * Apply the codec hardware settings that are required for intelligible
 * microphone audio on the Luckfox Pico Ultra W (RV1106 built-in codec).
 *
 * Root causes of bad default audio:
 *  - MICBIAS at minimum (VREFx0_8) → insufficient mic bias → distortion
 *  - DiffadcL mode → expects differential signal; onboard mic is single-ended
 *  - HPF disabled → DC offset + sub-bass rumble cause muffled/harsh output
 *  - MIC boost gain at 2 (not at maximum)
 *  - PGA gain at 0 dB (too low), no AGC
 *
 * All 11 amixer calls are batched into a single sh -c invocation to avoid
 * ~50 ms per fork/exec on the RV1106, which would otherwise add up to ~550 ms
 * of blocking startup delay while the AI MPI is already trying to buffer audio.
 *
 * References:
 *   https://wiki.luckfox.com/Luckfox-Pico-Ultra/Audio/  (FAQ section)
 */
static void codec_hw_init(int sample_rate)
{
    /* Select AGC sample rate string before building the command. */
    const char *agc_rate;
    if      (sample_rate >= 44100) agc_rate = "44.1KHz";
    else if (sample_rate >= 32000) agc_rate = "32KHz";
    else if (sample_rate >= 24000) agc_rate = "24KHz";
    else if (sample_rate >= 16000) agc_rate = "16KHz";
    else if (sample_rate >= 12000) agc_rate = "12KHz";
    else                           agc_rate = "8KHz";

    /*
     * Run all amixer cset commands in a single sh -c invocation (one fork).
     * Multiple sequential fork/exec calls add ~50 ms each on the RV1106 and
     * increase startup latency, which can contribute to initial AI overruns.
     * Using ';' so every command runs even if the previous one failed.
     *
     * All values are compile-time constants — no user input involved.
     */
    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd),
        "amixer cset name='ADC Main MICBIAS' On ;"
        "amixer cset name='ADC MICBIAS Voltage' VREFx0_975 ;"
        "amixer cset name='ADC Mode' SingadcL ;"
        "amixer cset name='ADC MIC Left Switch' Work ;"
        "amixer cset name='ADC MIC Left Gain' 3 ;"
        "amixer cset name='ADC ALC Left Volume' 23 ;"
        "amixer cset name='ADC HPF Cut-off' On ;"
        "amixer cset name='ALC AGC Left Max Volume' 7 ;"
        "amixer cset name='ALC AGC Left Min Volume' 0 ;"
        "amixer cset name='AGC Left Approximate Sample Rate' %s ;"
        "amixer cset name='ALC AGC Left Switch' On",
        agc_rate);

    if (n <= 0 || n >= (int)sizeof(cmd)) {
        printf("[AUDIO] codec_hw_init: command buffer overflow\n");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("[AUDIO] fork failed for codec hw init\n");
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        printf("[AUDIO] waitpid failed for codec hw init\n");
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("[AUDIO] codec hw init shell script non-zero exit (%d)\n",
               WEXITSTATUS(status));
    }

    printf("[AUDIO] codec hw init: MICBIAS=VREFx0_975 Mode=SingadcL "
           "HPF=On MIC_Gain=3 PGA=23 AGC=On rate=%s\n", agc_rate);
}


static int to_audio_sample_rate_enum(int sample_rate, AUDIO_SAMPLE_RATE_E *out) {
    if (!out) return -1;
    switch (sample_rate) {
        case 8000:  *out = AUDIO_SAMPLE_RATE_8000; break;
        case 16000: *out = AUDIO_SAMPLE_RATE_16000; break;
        default:
            return -1;
    }
    return 0;
}

static int calc_pt_num_per_frame(int sample_rate) {
    /* 20 ms packets are a robust default for RTP voice payloads. */
    return sample_rate / 50;
}

static int ai_enable_with_attr(audio_rtsp_ctx_t *ctx, int sample_rate, int channels) {
    AUDIO_SAMPLE_RATE_E en_sample_rate = AUDIO_SAMPLE_RATE_BUTT;
    if (to_audio_sample_rate_enum(sample_rate, &en_sample_rate) != 0) {
        return -1;
    }

    AIO_ATTR_S ai_attr;
    memset(&ai_attr, 0, sizeof(ai_attr));
    ai_attr.soundCard.channels = (RK_U32)channels;
    ai_attr.soundCard.sampleRate = (RK_U32)sample_rate;
    ai_attr.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    ai_attr.enSamplerate = en_sample_rate;
    ai_attr.enBitwidth = AUDIO_BIT_WIDTH_16;
    ai_attr.enSoundmode = (channels == 1) ? AUDIO_SOUND_MODE_MONO : AUDIO_SOUND_MODE_STEREO;
    ai_attr.u32FrmNum = 30;
    ai_attr.u32PtNumPerFrm = (RK_U32)calc_pt_num_per_frame(sample_rate);
    ai_attr.u32ChnCnt = (RK_U32)channels;
    (void)snprintf((char *)ai_attr.u8CardName, sizeof(ai_attr.u8CardName), "hw:0,0");

    int ret = RK_MPI_AI_SetPubAttr(ctx->ai_dev, &ai_attr);
    if (ret != RK_SUCCESS) {
        printf("[AUDIO] RK_MPI_AI_SetPubAttr failed (%d Hz, %d ch): 0x%x\n",
               sample_rate, channels, ret);
        return -1;
    }

    ret = RK_MPI_AI_Enable(ctx->ai_dev);
    if (ret != RK_SUCCESS) {
        printf("[AUDIO] RK_MPI_AI_Enable failed (%d Hz, %d ch): 0x%x\n",
               sample_rate, channels, ret);
        return -1;
    }

    ret = RK_MPI_AI_EnableChn(ctx->ai_dev, ctx->ai_chn);
    if (ret != RK_SUCCESS) {
        printf("[AUDIO] RK_MPI_AI_EnableChn failed (%d Hz, %d ch): 0x%x\n",
               sample_rate, channels, ret);
        RK_MPI_AI_Disable(ctx->ai_dev);
        return -1;
    }

    return 0;
}

}  // namespace

int audio_rtsp_init(audio_rtsp_ctx_t *ctx, int sample_rate, int channels) {
    if (!ctx) return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->ai_dev = 0;
    ctx->ai_chn = 0;
    ctx->aenc_chn = 0;
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;

    AUDIO_SAMPLE_RATE_E en_check = AUDIO_SAMPLE_RATE_BUTT;
    if (to_audio_sample_rate_enum(sample_rate, &en_check) != 0) {
        printf("[AUDIO] unsupported sample rate: %d (supported: 8000/16000)\n", sample_rate);
        return -1;
    }
    if (channels != 1 && channels != 2) {
        printf("[AUDIO] unsupported channels: %d (supported: 1/2)\n", channels);
        return -1;
    }

    /* Apply codec hardware settings before enabling the AI device.
     * Without this the board boots with defaults that produce muffled,
     * distorted audio (low MICBIAS, differential mode, HPF off, low gain). */
    codec_hw_init(sample_rate);

    int ret = -1;
    /*
     * RV1106 built-in codec does not support true mono (1ch) capture at the
     * hardware level — the I2S bus always transfers stereo frames even when
     * only the left ADC is active (SingadcL mode).  RK_MPI_AI_EnableChn
     * returns 0xa00a8010 for any 1ch config.  Prefer 2ch configurations and
     * only fall back to 1ch as a last resort for other boards.
     */
    const int fallback_cfg[][2] = {
        {sample_rate, 2},
        {sample_rate, channels},
        {16000, 2},
        {8000,  2},
        {8000,  1},
    };

    for (size_t i = 0; i < sizeof(fallback_cfg) / sizeof(fallback_cfg[0]); ++i) {
        const int try_rate = fallback_cfg[i][0];
        const int try_ch = fallback_cfg[i][1];

        /* Skip duplicate attempts. */
        int seen = 0;
        for (size_t j = 0; j < i; ++j) {
            if (fallback_cfg[j][0] == try_rate && fallback_cfg[j][1] == try_ch) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }

        ret = ai_enable_with_attr(ctx, try_rate, try_ch);
        if (ret == 0) {
            ctx->sample_rate = try_rate;
            ctx->channels = try_ch;
            break;
        }
    }

    if (ret != 0) {
        printf("[AUDIO] no compatible AI config found for this board/kernel\n");
        return -1;
    }

    AENC_CHN_ATTR_S aenc_attr;
    memset(&aenc_attr, 0, sizeof(aenc_attr));
    aenc_attr.enType = RK_AUDIO_ID_PCM_ALAW;
    aenc_attr.u32BufCount = 8;
    aenc_attr.u32Depth = 4;
    aenc_attr.stCodecAttr.enType = RK_AUDIO_ID_PCM_ALAW;
    aenc_attr.stCodecAttr.enBitwidth = AUDIO_BIT_WIDTH_16;
    aenc_attr.stCodecAttr.u32Channels = (RK_U32)ctx->channels;
    aenc_attr.stCodecAttr.u32SampleRate = (RK_U32)ctx->sample_rate;
    aenc_attr.stCodecAttr.u32BitPerCodedSample = 8;
    aenc_attr.stCodecAttr.u32Bitrate = (RK_U32)(ctx->sample_rate * ctx->channels * 8);

    ret = RK_MPI_AENC_CreateChn(ctx->aenc_chn, &aenc_attr);
    if (ret != RK_SUCCESS) {
        printf("[AUDIO] RK_MPI_AENC_CreateChn failed: 0x%x\n", ret);
        RK_MPI_AI_DisableChn(ctx->ai_dev, ctx->ai_chn);
        RK_MPI_AI_Disable(ctx->ai_dev);
        return -1;
    }

    MPP_CHN_S src;
    MPP_CHN_S dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    src.enModId = RK_ID_AI;
    src.s32DevId = ctx->ai_dev;
    src.s32ChnId = ctx->ai_chn;

    dst.enModId = RK_ID_AENC;
    dst.s32DevId = 0;
    dst.s32ChnId = ctx->aenc_chn;

    ret = RK_MPI_SYS_Bind(&src, &dst);
    if (ret != RK_SUCCESS) {
        printf("[AUDIO] RK_MPI_SYS_Bind(AI->AENC) failed: 0x%x\n", ret);
        RK_MPI_AENC_DestroyChn(ctx->aenc_chn);
        RK_MPI_AI_DisableChn(ctx->ai_dev, ctx->ai_chn);
        RK_MPI_AI_Disable(ctx->ai_dev);
        return -1;
    }

    ctx->bound = 1;
    ctx->initialized = 1;

        printf("[AUDIO] enabled: codec=G711A sample_rate=%d channels=%d card=hw:0,0\n",
            ctx->sample_rate, ctx->channels);
    return 0;
}

int audio_rtsp_send_pending(audio_rtsp_ctx_t *ctx,
                            rtsp_session_handle rtsp_session,
                            int max_packets) {
    if (!ctx || !ctx->initialized || !rtsp_session || max_packets <= 0) {
        return 0;
    }

    int sent = 0;
    for (int i = 0; i < max_packets; ++i) {
        AUDIO_STREAM_S stream;
        memset(&stream, 0, sizeof(stream));

        int ret = RK_MPI_AENC_GetStream(ctx->aenc_chn, &stream, 0);
        if (ret != RK_SUCCESS) {
            break;
        }

        void *data = RK_MPI_MB_Handle2VirAddr(stream.pMbBlk);
        if (data && stream.u32Len > 0) {
            rtsp_tx_audio(rtsp_session,
                          (const uint8_t *)data,
                          (int)stream.u32Len,
                          stream.u64TimeStamp);
            sent++;
        }

        ret = RK_MPI_AENC_ReleaseStream(ctx->aenc_chn, &stream);
        if (ret != RK_SUCCESS) {
            printf("[AUDIO] RK_MPI_AENC_ReleaseStream failed: 0x%x\n", ret);
            break;
        }
    }

    return sent;
}

void audio_rtsp_deinit(audio_rtsp_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->bound) {
        MPP_CHN_S src;
        MPP_CHN_S dst;
        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));

        src.enModId = RK_ID_AI;
        src.s32DevId = ctx->ai_dev;
        src.s32ChnId = ctx->ai_chn;

        dst.enModId = RK_ID_AENC;
        dst.s32DevId = 0;
        dst.s32ChnId = ctx->aenc_chn;

        int ret = RK_MPI_SYS_UnBind(&src, &dst);
        if (ret != RK_SUCCESS) {
            printf("[AUDIO] RK_MPI_SYS_UnBind(AI->AENC) failed: 0x%x\n", ret);
        }
        ctx->bound = 0;
    }

    if (ctx->initialized) {
        int ret = RK_MPI_AENC_DestroyChn(ctx->aenc_chn);
        if (ret != RK_SUCCESS) {
            printf("[AUDIO] RK_MPI_AENC_DestroyChn failed: 0x%x\n", ret);
        }

        ret = RK_MPI_AI_DisableChn(ctx->ai_dev, ctx->ai_chn);
        if (ret != RK_SUCCESS) {
            printf("[AUDIO] RK_MPI_AI_DisableChn failed: 0x%x\n", ret);
        }

        ret = RK_MPI_AI_Disable(ctx->ai_dev);
        if (ret != RK_SUCCESS) {
            printf("[AUDIO] RK_MPI_AI_Disable failed: 0x%x\n", ret);
        }

        ctx->initialized = 0;
        printf("[AUDIO] stopped\n");
    }
}
