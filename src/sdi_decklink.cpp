#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sdi_decklink.h"
#include "logger.h"

#include <DeckLinkAPI.h>

#include <mutex>
#include <queue>
#include <condition_variable>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/buffer.h>
#include <libswscale/swscale.h>
}

/* Zero-copy custom free callbacks for DeckLink AVFrames */
static void decklink_video_frame_free(void *opaque, uint8_t *data) {
    (void)data;
    IDeckLinkVideoInputFrame *dlFrame = (IDeckLinkVideoInputFrame*)opaque;
    if (dlFrame) {
        dlFrame->Release();
    }
}

static void decklink_audio_packet_free(void *opaque, uint8_t *data) {
    (void)data;
    IDeckLinkAudioInputPacket *dlPacket = (IDeckLinkAudioInputPacket*)opaque;
    if (dlPacket) {
        dlPacket->Release();
    }
}

/* Helper to convert BMD string to C string */
static std::string bmd_to_string(const char *str) {
    return str ? std::string(str) : "";
}

/* Find display mode by name or best match */
static BMDDisplayMode find_display_mode(const char *mode_name, int *out_w, int *out_h, AVRational *out_fps) {
    if (!mode_name || strcasecmp(mode_name, "auto") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){60000, 1001};
        return bmdModeHD1080p5994;
    }
    if (strcasecmp(mode_name, "1080p60") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){60, 1};
        return bmdModeHD1080p6000;
    }
    if (strcasecmp(mode_name, "1080p5994") == 0 || strcasecmp(mode_name, "1080p59.94") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){60000, 1001};
        return bmdModeHD1080p5994;
    }
    if (strcasecmp(mode_name, "1080p50") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){50, 1};
        return bmdModeHD1080p50;
    }
    if (strcasecmp(mode_name, "1080p30") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){30, 1};
        return bmdModeHD1080p30;
    }
    if (strcasecmp(mode_name, "1080p2997") == 0 || strcasecmp(mode_name, "1080p29.97") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){30000, 1001};
        return bmdModeHD1080p2997;
    }
    if (strcasecmp(mode_name, "1080p25") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){25, 1};
        return bmdModeHD1080p25;
    }
    if (strcasecmp(mode_name, "1080i50") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){25, 1};
        return bmdModeHD1080i50;
    }
    if (strcasecmp(mode_name, "1080i5994") == 0 || strcasecmp(mode_name, "1080i59.94") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){30000, 1001};
        return bmdModeHD1080i5994;
    }
    if (strcasecmp(mode_name, "1080i60") == 0) {
        if (out_w) *out_w = 1920;
        if (out_h) *out_h = 1080;
        if (out_fps) *out_fps = (AVRational){30, 1};
        return bmdModeHD1080i6000;
    }
    if (strcasecmp(mode_name, "720p60") == 0) {
        if (out_w) *out_w = 1280;
        if (out_h) *out_h = 720;
        if (out_fps) *out_fps = (AVRational){60, 1};
        return bmdModeHD720p60;
    }
    if (strcasecmp(mode_name, "720p5994") == 0 || strcasecmp(mode_name, "720p59.94") == 0) {
        if (out_w) *out_w = 1280;
        if (out_h) *out_h = 720;
        if (out_fps) *out_fps = (AVRational){60000, 1001};
        return bmdModeHD720p5994;
    }
    if (strcasecmp(mode_name, "720p50") == 0) {
        if (out_w) *out_w = 1280;
        if (out_h) *out_h = 720;
        if (out_fps) *out_fps = (AVRational){50, 1};
        return bmdModeHD720p50;
    }

    if (out_w) *out_w = 1920;
    if (out_h) *out_h = 1080;
    if (out_fps) *out_fps = (AVRational){60000, 1001};
    return bmdModeHD1080p5994;
}

/* Match display mode by resolution and framerate */
static BMDDisplayMode match_output_mode(int width, int height, AVRational fps) {
    double rfps = (fps.den > 0) ? (double)fps.num / fps.den : 60.0;

    if (width == 1920 && height == 1080) {
        if (rfps > 59.5 && rfps < 60.0) return bmdModeHD1080p5994;
        if (rfps >= 60.0) return bmdModeHD1080p6000;
        if (rfps >= 49.5 && rfps <= 50.5) return bmdModeHD1080p50;
        if (rfps > 29.5 && rfps < 30.0) return bmdModeHD1080p2997;
        if (rfps >= 30.0) return bmdModeHD1080p30;
        if (rfps >= 24.5 && rfps <= 25.5) return bmdModeHD1080p25;
        if (rfps >= 23.5 && rfps <= 24.2) return bmdModeHD1080p24;
        return bmdModeHD1080p5994;
    }
    if (width == 1280 && height == 720) {
        if (rfps > 59.5 && rfps < 60.0) return bmdModeHD720p5994;
        if (rfps >= 60.0) return bmdModeHD720p60;
        if (rfps >= 49.5 && rfps <= 50.5) return bmdModeHD720p50;
        return bmdModeHD720p60;
    }
    if (width == 3840 && height == 2160) {
        if (rfps > 59.5 && rfps < 60.0) return bmdMode4K2160p5994;
        if (rfps >= 60.0) return bmdMode4K2160p60;
        if (rfps >= 49.5 && rfps <= 50.5) return bmdMode4K2160p50;
        if (rfps > 29.5 && rfps < 30.0) return bmdMode4K2160p2997;
        if (rfps >= 30.0) return bmdMode4K2160p30;
        if (rfps >= 24.5 && rfps <= 25.5) return bmdMode4K2160p25;
        return bmdMode4K2160p5994;
    }
    return bmdModeHD1080p5994;
}

/* =========================================================================
 * DeckLink Device Discovery Implementation
 * ========================================================================= */

int sdi_list_devices(SdiDeviceInfo *devices, int max_devices) {
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) {
        LOG_WARN("[SDI] No Blackmagic DeckLink driver detected on this system.");
        return 0;
    }

    printf("\n\033[1;36m========================================================================\033[0m\n");
    printf("\033[1;36m   Blackmagic DeckLink SDI Devices (DeckLink SDK v14.2)\033[0m\n");
    printf("\033[1;36m========================================================================\033[0m\n");

    int count = 0;
    IDeckLink *deckLink = NULL;

    while (iterator->Next(&deckLink) == S_OK) {
        count++;
        const char *deviceNameStr = NULL;
        deckLink->GetDisplayName(&deviceNameStr);
        std::string devName = bmd_to_string(deviceNameStr);

        IDeckLinkProfileAttributes *attrs = NULL;
        bool hasInput = false;
        bool hasOutput = false;
        int64_t ioPorts = 0;

        if (deckLink->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attrs) == S_OK) {
            attrs->GetInt(BMDDeckLinkVideoInputConnections, &ioPorts);
            hasInput = (ioPorts != 0);
            attrs->GetInt(BMDDeckLinkVideoOutputConnections, &ioPorts);
            hasOutput = (ioPorts != 0);
            attrs->Release();
        }

        IDeckLinkStatus *status = NULL;
        bool signalLocked = false;
        char signalMode[64] = "No Signal / Disconnected";

        if (deckLink->QueryInterface(IID_IDeckLinkStatus, (void**)&status) == S_OK) {
            status->GetFlag(bmdDeckLinkStatusVideoInputSignalLocked, &signalLocked);
            if (signalLocked) {
                int64_t lockedMode = 0;
                if (status->GetInt(bmdDeckLinkStatusDetectedVideoInputMode, &lockedMode) == S_OK ||
                    status->GetInt(bmdDeckLinkStatusCurrentVideoInputMode, &lockedMode) == S_OK) {
                    IDeckLinkInput *dInput = NULL;
                    if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&dInput) == S_OK) {
                        IDeckLinkDisplayMode *dm = NULL;
                        if (dInput->GetDisplayMode((BMDDisplayMode)lockedMode, &dm) == S_OK) {
                            const char *mName = NULL;
                            dm->GetName(&mName);
                            snprintf(signalMode, sizeof(signalMode), "%s (LOCKED)", mName ? mName : "Unknown");
                            dm->Release();
                        }
                        dInput->Release();
                    }
                }
            }
            status->Release();
        }

        printf("  [%d] \033[1;32m%-22s\033[0m | I/O: [%s%s%s] | Status: %s\n",
               count, devName.c_str(),
               hasInput ? "IN" : "",
               (hasInput && hasOutput) ? "/" : "",
               hasOutput ? "OUT" : "",
               signalLocked ? "\033[1;32mLOCKED SIGNAL\033[0m" : "\033[0;33mNO CARRIER\033[0m");

        if (devices && count <= max_devices) {
            SdiDeviceInfo *d = &devices[count - 1];
            d->index = count;
            snprintf(d->name, sizeof(d->name), "%s", devName.c_str());
            snprintf(d->model, sizeof(d->model), "%s", devName.c_str());
            d->supports_input = hasInput;
            d->supports_output = hasOutput;
            d->has_input_signal = signalLocked;
            snprintf(d->detected_mode_name, sizeof(d->detected_mode_name), "%s", signalMode);
        }

        deckLink->Release();
    }

    iterator->Release();

    if (count == 0) {
        printf("  \033[0;33mNo Blackmagic DeckLink hardware cards found.\033[0m\n");
    }
    printf("\033[1;36m========================================================================\033[0m\n\n");

    return count;
}

/* =========================================================================
 * DeckLink SDI Input Implementation (Capture)
 * ========================================================================= */

class DeckLinkCaptureDelegate : public IDeckLinkInputCallback {
public:
    DeckLinkCaptureDelegate(SdiInputContext *owner) : m_owner(owner), m_refCount(1) {}

    HRESULT QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG AddRef() override { return ++m_refCount; }
    ULONG Release() override {
        ULONG cnt = --m_refCount;
        if (cnt == 0) delete this;
        return cnt;
    }

    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                                    IDeckLinkDisplayMode *newDisplayMode,
                                    BMDDetectedVideoInputFormatFlags detectedSignalFlags) override;

    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                   IDeckLinkAudioInputPacket* audioPacket) override;

private:
    SdiInputContext *m_owner;
    std::atomic<ULONG> m_refCount;
};

struct SdiInputContext {
    IDeckLink *deckLink;
    IDeckLinkInput *deckLinkInput;
    DeckLinkCaptureDelegate *delegate;

    std::string deviceName;
    bool testBars;

    int width;
    int height;
    AVRational framerate;
    BMDDisplayMode currentMode;
    int audioChannels;

    std::mutex videoMutex;
    std::condition_variable videoCv;
    std::queue<AVFrame*> videoQueue;

    std::mutex audioMutex;
    std::condition_variable audioCv;
    std::queue<AVFrame*> audioQueue;

    std::atomic<bool> running;
    int64_t frameCount;
    int64_t lastFrameTimeUs;

    std::atomic<bool> formatLocked;
    std::condition_variable formatCv;
    std::mutex formatMutex;

    int64_t firstStreamTime;
    int64_t lastRawStreamTime;
    int64_t streamTimeOffset;
    int64_t lastAssignedPts;

    SdiSyncType syncType;
    int64_t audioSampleCount;
    int64_t firstAudioPacketTime;
};

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents,
    IDeckLinkDisplayMode *newDisplayMode,
    BMDDetectedVideoInputFormatFlags)
{
    if (!newDisplayMode || !m_owner) return S_OK;

    BMDDisplayMode newMode = newDisplayMode->GetDisplayMode();
    int newWidth = newDisplayMode->GetWidth();
    int newHeight = newDisplayMode->GetHeight();

    /* Avoid continuous pause/restart cycle if mode is already active */
    if (m_owner->currentMode == newMode &&
        m_owner->width == newWidth &&
        m_owner->height == newHeight &&
        m_owner->formatLocked.load()) {
        return S_OK;
    }

    const char *modeName = NULL;
    newDisplayMode->GetName(&modeName);
    BMDTimeValue val = 0;
    BMDTimeScale scale = 0;
    newDisplayMode->GetFrameRate(&val, &scale);
    AVRational newFps = (AVRational){(int)scale, (int)val};

    LOG_INFO("[SDI IN] Video input format detected / locked: %s (%dx%d @ %.2f fps)",
             modeName ? modeName : "Unknown", newWidth, newHeight,
             (val > 0) ? (double)scale / val : 0.0);

    m_owner->width = newWidth;
    m_owner->height = newHeight;
    m_owner->framerate = newFps;
    m_owner->currentMode = newMode;

    /* Flush any stale queues from previous mode and reset clock origin */
    sdi_input_flush(m_owner);

    /* Mark format as locked AFTER setting width, height, and framerate */
    {
        std::unique_lock<std::mutex> flock(m_owner->formatMutex);
        m_owner->formatLocked = true;
        m_owner->formatCv.notify_all();
    }

    m_owner->deckLinkInput->PauseStreams();
    m_owner->deckLinkInput->EnableVideoInput(m_owner->currentMode, bmdFormat8BitYUV,
                                            bmdVideoInputEnableFormatDetection);
    m_owner->deckLinkInput->StartStreams();
    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame* videoFrame,
    IDeckLinkAudioInputPacket* audioPacket)
{
    if (!m_owner || !m_owner->running) return S_OK;

    int64_t nowUs = av_gettime_relative();

    if (videoFrame) {
        bool hasSignal = !(videoFrame->GetFlags() & bmdFrameHasNoInputSource);
        if (hasSignal) {
            /* Detect exact hardware frame duration and dimensions directly from the video frame */
            BMDTimeValue ft = 0, fd = 0;
            if (videoFrame->GetStreamTime(&ft, &fd, 120000) == S_OK && fd > 0) {
                AVRational exactFps;
                if (fd == 2002) exactFps = (AVRational){60000, 1001};       /* 59.94p */
                else if (fd == 2000) exactFps = (AVRational){60, 1};         /* 60.00p */
                else if (fd == 2400) exactFps = (AVRational){50, 1};         /* 50.00p */
                else if (fd == 4004) exactFps = (AVRational){30000, 1001};   /* 29.97p / 59.94i */
                else if (fd == 4000) exactFps = (AVRational){30, 1};         /* 30.00p / 60.00i */
                else if (fd == 4800) exactFps = (AVRational){25, 1};         /* 25.00p / 50.00i */
                else if (fd == 5005) exactFps = (AVRational){24000, 1001};   /* 23.976p */
                else if (fd == 5000) exactFps = (AVRational){24, 1};         /* 24.00p */
                else exactFps = (AVRational){(int)(120000 / fd), 1};

                if (m_owner->framerate.num != exactFps.num || m_owner->framerate.den != exactFps.den) {
                    LOG_INFO("[SDI IN] Hardware frame timing detected: %dx%d @ %.2f fps (duration %lld/120000)",
                             videoFrame->GetWidth(), videoFrame->GetHeight(),
                             (double)exactFps.num / exactFps.den, (long long)fd);
                    m_owner->framerate = exactFps;
                }
                m_owner->width = videoFrame->GetWidth();
                m_owner->height = videoFrame->GetHeight();
            }

            if (!m_owner->formatLocked) {
                std::unique_lock<std::mutex> flock(m_owner->formatMutex);
                m_owner->formatLocked = true;
                m_owner->formatCv.notify_all();
            }

            void *frameBytes = NULL;
            if (videoFrame->GetBytes(&frameBytes) == S_OK && frameBytes) {
                int w = videoFrame->GetWidth();
                int h = videoFrame->GetHeight();
                int rowBytes = videoFrame->GetRowBytes();

                AVFrame *f = av_frame_alloc();
                if (f) {
                    f->format = AV_PIX_FMT_UYVY422;
                    f->width = w;
                    f->height = h;
                    f->linesize[0] = rowBytes;
                    f->data[0] = (uint8_t*)frameBytes;

                    videoFrame->AddRef();
                    f->buf[0] = av_buffer_create((uint8_t*)frameBytes, (size_t)rowBytes * h,
                                                 decklink_video_frame_free, videoFrame, 0);
                    if (f->buf[0]) {
                        f->pts = m_owner->frameCount++;

                        std::unique_lock<std::mutex> lock(m_owner->videoMutex);
                        while (m_owner->videoQueue.size() >= 120) {
                            AVFrame *dropped = m_owner->videoQueue.front();
                            m_owner->videoQueue.pop();
                            av_frame_free(&dropped);
                        }
                        m_owner->videoQueue.push(f);
                        m_owner->videoCv.notify_one();
                        m_owner->lastFrameTimeUs = nowUs;
                    } else {
                        videoFrame->Release();
                        av_frame_free(&f);
                    }
                }
            }
        } else if (m_owner->testBars) {
            /* Render SMPTE 75% Color Bars in UYVY format for test */
            int w = videoFrame->GetWidth() > 0 ? videoFrame->GetWidth() : m_owner->width;
            int h = videoFrame->GetHeight() > 0 ? videoFrame->GetHeight() : m_owner->height;
            if (w <= 0) w = 1920;
            if (h <= 0) h = 1080;

            AVFrame *f = av_frame_alloc();
            if (f) {
                f->format = AV_PIX_FMT_UYVY422;
                f->width = w;
                f->height = h;

                if (av_frame_get_buffer(f, 32) == 0) {
                    static const uint8_t bars_uyvy[8][4] = {
                        {128, 235, 128, 235}, /* 75% White */
                        { 16, 210, 146, 210}, /* Yellow */
                        {166, 170,  16, 170}, /* Cyan */
                        { 54, 145,  34, 145}, /* Green */
                        {202, 106, 222, 106}, /* Magenta */
                        { 90,  81, 240,  81}, /* Red */
                        {240,  41, 110,  41}, /* Blue */
                        {128,  16, 128,  16}  /* Black */
                    };
                    int bar_w = (w / 8) & ~1;
                    if (bar_w <= 0) bar_w = 2;

                    for (int y = 0; y < h; y++) {
                        uint8_t *row = f->data[0] + y * f->linesize[0];
                        for (int x = 0; x < w; x += 2) {
                            int bar_idx = x / bar_w;
                            if (bar_idx > 7) bar_idx = 7;
                            row[x * 2 + 0] = bars_uyvy[bar_idx][0];
                            row[x * 2 + 1] = bars_uyvy[bar_idx][1];
                            row[x * 2 + 2] = bars_uyvy[bar_idx][2];
                            row[x * 2 + 3] = bars_uyvy[bar_idx][3];
                        }
                    }

                    f->pts = m_owner->frameCount++;

                    std::unique_lock<std::mutex> lock(m_owner->videoMutex);
                    while (m_owner->videoQueue.size() >= 120) {
                        AVFrame *dropped = m_owner->videoQueue.front();
                        m_owner->videoQueue.pop();
                        av_frame_free(&dropped);
                    }
                    m_owner->videoQueue.push(f);
                    m_owner->videoCv.notify_one();
                    m_owner->lastFrameTimeUs = nowUs;
                } else {
                    av_frame_free(&f);
                }
            }
        } else {
            static int64_t last_warn_us = 0;
            if (nowUs - last_warn_us > 3000000) {
                LOG_WARN("[SDI IN] Waiting for video signal on '%s'... (Status: NO CARRIER). Pass --sdi-test-bars to test without live feed.",
                         m_owner->deviceName.c_str());
                last_warn_us = nowUs;
            }
        }
    }

    /* Ingest audio frames with monotonic sample counter matching video start */
    if (audioPacket && m_owner->audioChannels > 0) {
        void *audioBytes = NULL;
        if (audioPacket->GetBytes(&audioBytes) == S_OK && audioBytes) {
            uint32_t sampleCount = audioPacket->GetSampleFrameCount();
            if (sampleCount > 0) {
                AVFrame *af = av_frame_alloc();
                if (af) {
                    af->format = AV_SAMPLE_FMT_S16;
                    af->sample_rate = 48000;
                    if (m_owner->audioChannels == 1) {
                        af->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
                    } else {
                        af->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
                    }
                    af->nb_samples = sampleCount;
                    int bytesToCopy = sampleCount * m_owner->audioChannels * sizeof(int16_t);
                    af->linesize[0] = bytesToCopy;
                    af->data[0] = (uint8_t*)audioBytes;

                    audioPacket->AddRef();
                    af->buf[0] = av_buffer_create((uint8_t*)audioBytes, bytesToCopy,
                                                  decklink_audio_packet_free, audioPacket, 0);
                    if (af->buf[0]) {
                        af->pts = m_owner->audioSampleCount;
                        m_owner->audioSampleCount += sampleCount;

                        std::unique_lock<std::mutex> lock(m_owner->audioMutex);
                        while (m_owner->audioQueue.size() >= 240) {
                            AVFrame *dropped = m_owner->audioQueue.front();
                            m_owner->audioQueue.pop();
                            av_frame_free(&dropped);
                        }
                        m_owner->audioQueue.push(af);
                        m_owner->audioCv.notify_one();
                    } else {
                        audioPacket->Release();
                        av_frame_free(&af);
                    }
                }
            }
        }
    }

    return S_OK;
}

SdiInputContext *sdi_input_open(const char *device_id, const char *mode_name, int audio_channels) {
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) {
        LOG_ERROR("[SDI IN] Unable to create DeckLink iterator");
        return NULL;
    }

    IDeckLink *targetDeckLink = NULL;
    int targetIdx = atoi(device_id ? device_id : "1");
    int currentIdx = 0;
    IDeckLink *d = NULL;

    while (iterator->Next(&d) == S_OK) {
        currentIdx++;
        const char *name = NULL;
        d->GetDisplayName(&name);
        std::string dName = bmd_to_string(name);

        if ((targetIdx > 0 && currentIdx == targetIdx) ||
            (device_id && dName.find(device_id) != std::string::npos)) {
            targetDeckLink = d;
            LOG_INFO("[SDI IN] Selected DeckLink Device #%d: '%s'", currentIdx, dName.c_str());
            break;
        }
        d->Release();
    }
    iterator->Release();

    if (!targetDeckLink) {
        LOG_ERROR("[SDI IN] Blackmagic device '%s' not found!", device_id ? device_id : "default");
        return NULL;
    }

    IDeckLinkInput *input = NULL;
    if (targetDeckLink->QueryInterface(IID_IDeckLinkInput, (void**)&input) != S_OK) {
        LOG_ERROR("[SDI IN] Device does not support video input / capture!");
        targetDeckLink->Release();
        return NULL;
    }

    SdiInputContext *ctx = new SdiInputContext();
    ctx->deckLink = targetDeckLink;
    ctx->deckLinkInput = input;
    ctx->delegate = new DeckLinkCaptureDelegate(ctx);
    ctx->deviceName = device_id ? device_id : "DeckLink";
    ctx->testBars = false;
    ctx->audioChannels = (audio_channels > 0) ? audio_channels : 2;
    ctx->frameCount = 0;
    ctx->lastFrameTimeUs = 0;
    ctx->running = false;
    ctx->formatLocked = false;
    ctx->firstStreamTime = -1;
    ctx->lastRawStreamTime = -1;
    ctx->streamTimeOffset = 0;
    ctx->lastAssignedPts = -1;
    ctx->syncType = SDI_SYNC_HARDWARE;
    ctx->audioSampleCount = 0;
    ctx->firstAudioPacketTime = -1;

    int defaultW = 1920, defaultH = 1080;
    AVRational defaultFps = (AVRational){60000, 1001};
    ctx->currentMode = find_display_mode(mode_name, &defaultW, &defaultH, &defaultFps);
    ctx->width = defaultW;
    ctx->height = defaultH;
    ctx->framerate = defaultFps;

    input->SetCallback(ctx->delegate);

    BMDVideoInputFlags flags = bmdVideoInputEnableFormatDetection;
    HRESULT res = input->EnableVideoInput(ctx->currentMode, bmdFormat8BitYUV, flags);
    if (res != S_OK) {
        LOG_ERROR("[SDI IN] EnableVideoInput failed with HRESULT 0x%08x", (unsigned int)res);
        sdi_input_close(&ctx);
        return NULL;
    }

    if (ctx->audioChannels > 0) {
        input->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, ctx->audioChannels);
    }

    LOG_INFO("[SDI IN] Successfully configured port for capture (%dx%d, Auto-Format Detection active)",
             ctx->width, ctx->height);

    return ctx;
}

void sdi_input_set_sync_type(SdiInputContext *ctx, SdiSyncType sync_type) {
    if (ctx) {
        ctx->syncType = sync_type;
        LOG_INFO("[SDI IN] A/V Sync Mode configured: %s",
                 (sync_type == SDI_SYNC_SELF) ? "SELF-GENERATED (Pure Monotonic Frame/Sample Sequence)" :
                                                "HARDWARE CLOCK (Blackmagic Stream Clock)");
    }
}

void sdi_input_set_test_bars(SdiInputContext *ctx, bool enable) {
    if (ctx) {
        ctx->testBars = enable;
        if (enable) {
            LOG_INFO("[SDI IN] Test bars generator enabled for '%s' (fallback on NO CARRIER).",
                     ctx->deviceName.c_str());
        }
    }
}

int sdi_input_start(SdiInputContext *ctx) {
    if (!ctx || !ctx->deckLinkInput) return -1;
    ctx->running = true;
    HRESULT res = ctx->deckLinkInput->StartStreams();
    if (res != S_OK) {
        LOG_ERROR("[SDI IN] StartStreams failed: 0x%08x", (unsigned int)res);
        return -1;
    }
    LOG_INFO("[SDI IN] Hardware capture stream started.");
    return 0;
}

int sdi_input_read_video_frame(SdiInputContext *ctx, AVFrame **out_frame, int timeout_ms) {
    if (!ctx || !out_frame) return -1;
    std::unique_lock<std::mutex> lock(ctx->videoMutex);
    if (ctx->videoQueue.empty()) {
        if (!ctx->videoCv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [ctx]() { return !ctx->videoQueue.empty() || !ctx->running; })) {
            return AVERROR(EAGAIN);
        }
    }
    if (!ctx->running && ctx->videoQueue.empty()) return AVERROR_EOF;

    *out_frame = ctx->videoQueue.front();
    ctx->videoQueue.pop();
    return 0;
}

int sdi_input_read_audio_frame(SdiInputContext *ctx, AVFrame **out_frame, int timeout_ms) {
    if (!ctx || !out_frame) return -1;
    std::unique_lock<std::mutex> lock(ctx->audioMutex);
    if (ctx->audioQueue.empty()) {
        if (timeout_ms <= 0) return AVERROR(EAGAIN);
        if (!ctx->audioCv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [ctx]() { return !ctx->audioQueue.empty() || !ctx->running; })) {
            return AVERROR(EAGAIN);
        }
    }
    if (!ctx->running && ctx->audioQueue.empty()) return AVERROR_EOF;

    *out_frame = ctx->audioQueue.front();
    ctx->audioQueue.pop();
    return 0;
}

int sdi_input_get_format(SdiInputContext *ctx, int *width, int *height, AVRational *framerate) {
    if (!ctx) return -1;
    if (width) *width = ctx->width;
    if (height) *height = ctx->height;
    if (framerate) *framerate = ctx->framerate;
    return 0;
}

int sdi_input_wait_for_format(SdiInputContext *ctx, int timeout_ms) {
    if (!ctx) return -1;
    if (ctx->formatLocked) return 0;
    std::unique_lock<std::mutex> lock(ctx->formatMutex);
    ctx->formatCv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [ctx]() { return ctx->formatLocked.load() || !ctx->running; });
    return ctx->formatLocked ? 0 : -1;
}

void sdi_input_flush(SdiInputContext *ctx) {
    if (!ctx) return;
    {
        std::unique_lock<std::mutex> lock(ctx->videoMutex);
        while (!ctx->videoQueue.empty()) {
            AVFrame *f = ctx->videoQueue.front();
            ctx->videoQueue.pop();
            av_frame_free(&f);
        }
    }
    {
        std::unique_lock<std::mutex> lock(ctx->audioMutex);
        while (!ctx->audioQueue.empty()) {
            AVFrame *af = ctx->audioQueue.front();
            ctx->audioQueue.pop();
            av_frame_free(&af);
        }
    }
    ctx->firstStreamTime = -1;
    ctx->lastRawStreamTime = -1;
    ctx->streamTimeOffset = 0;
    ctx->lastAssignedPts = -1;
    ctx->audioSampleCount = 0;
    ctx->firstAudioPacketTime = -1;
    ctx->frameCount = 0;
}

void sdi_input_close(SdiInputContext **ctx_ptr) {
    if (!ctx_ptr || !*ctx_ptr) return;
    SdiInputContext *ctx = *ctx_ptr;
    ctx->running = false;

    if (ctx->deckLinkInput) {
        ctx->deckLinkInput->StopStreams();
        ctx->deckLinkInput->DisableVideoInput();
        ctx->deckLinkInput->DisableAudioInput();
        ctx->deckLinkInput->SetCallback(NULL);
        ctx->deckLinkInput->Release();
    }
    if (ctx->delegate) {
        ctx->delegate->Release();
    }
    if (ctx->deckLink) {
        ctx->deckLink->Release();
    }

    sdi_input_flush(ctx);

    delete ctx;
    *ctx_ptr = NULL;
    LOG_INFO("[SDI IN] Capture port closed cleanly.");
}

/* =========================================================================
 * DeckLink SDI Output Implementation (Playout)
 * ========================================================================= */

struct SdiOutputContext {
    IDeckLink *deckLink;
    IDeckLinkOutput *deckLinkOutput;

    int width;
    int height;
    AVRational framerate;
    BMDDisplayMode displayMode;
    int audioChannels;

    struct SwsContext *swsCtx;
    int swsSrcW;
    int swsSrcH;
    int swsSrcFmt;

    int64_t framesWritten;
};

SdiOutputContext *sdi_output_open(const char *device_id, int width, int height,
                                  AVRational framerate, int audio_channels) {
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) {
        LOG_ERROR("[SDI OUT] Unable to create DeckLink iterator");
        return NULL;
    }

    IDeckLink *targetDeckLink = NULL;
    int targetIdx = atoi(device_id ? device_id : "2");
    int currentIdx = 0;
    IDeckLink *d = NULL;

    while (iterator->Next(&d) == S_OK) {
        currentIdx++;
        const char *name = NULL;
        d->GetDisplayName(&name);
        std::string dName = bmd_to_string(name);

        if ((targetIdx > 0 && currentIdx == targetIdx) ||
            (device_id && dName.find(device_id) != std::string::npos)) {
            targetDeckLink = d;
            LOG_INFO("[SDI OUT] Selected DeckLink Playout Device #%d: '%s'", currentIdx, dName.c_str());
            break;
        }
        d->Release();
    }
    iterator->Release();

    if (!targetDeckLink) {
        LOG_ERROR("[SDI OUT] Blackmagic playout device '%s' not found!", device_id ? device_id : "default");
        return NULL;
    }

    IDeckLinkOutput *output = NULL;
    if (targetDeckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&output) != S_OK) {
        LOG_ERROR("[SDI OUT] Device does not support video output / playout!");
        targetDeckLink->Release();
        return NULL;
    }

    SdiOutputContext *ctx = new SdiOutputContext();
    ctx->deckLink = targetDeckLink;
    ctx->deckLinkOutput = output;
    ctx->width = (width > 0) ? width : 1920;
    ctx->height = (height > 0) ? height : 1080;
    ctx->framerate = (framerate.den > 0) ? framerate : (AVRational){60000, 1001};
    ctx->audioChannels = (audio_channels > 0) ? audio_channels : 2;
    ctx->swsCtx = NULL;
    ctx->swsSrcW = 0;
    ctx->swsSrcH = 0;
    ctx->swsSrcFmt = -1;
    ctx->framesWritten = 0;

    ctx->displayMode = match_output_mode(ctx->width, ctx->height, ctx->framerate);

    HRESULT res = output->EnableVideoOutput(ctx->displayMode, bmdVideoOutputFlagDefault);
    if (res != S_OK) {
        LOG_ERROR("[SDI OUT] EnableVideoOutput failed: 0x%08x", (unsigned int)res);
        sdi_output_close(&ctx);
        return NULL;
    }

    if (ctx->audioChannels > 0) {
        output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger,
                                  ctx->audioChannels, bmdAudioOutputStreamContinuous);
    }

    LOG_INFO("[SDI OUT] Playout port active: %dx%d (DisplayMode 0x%08x)",
             ctx->width, ctx->height, (unsigned int)ctx->displayMode);

    return ctx;
}

int sdi_output_write_video_frame(SdiOutputContext *ctx, const AVFrame *frame) {
    if (!ctx || !ctx->deckLinkOutput || !frame) return -1;

    int w = ctx->width;
    int h = ctx->height;
    int rowBytes = w * 2; /* UYVY 8-bit YUV = 2 bytes per pixel */

    IDeckLinkMutableVideoFrame *bmdFrame = NULL;
    HRESULT res = ctx->deckLinkOutput->CreateVideoFrame(w, h, rowBytes, bmdFormat8BitYUV,
                                                        bmdFrameFlagDefault, &bmdFrame);
    if (res != S_OK || !bmdFrame) {
        LOG_ERROR("[SDI OUT] CreateVideoFrame failed: 0x%08x", (unsigned int)res);
        return -1;
    }

    void *bmdBytes = NULL;
    bmdFrame->GetBytes(&bmdBytes);

    if (frame->format == AV_PIX_FMT_UYVY422 && frame->width == w && frame->height == h) {
        /* Direct fast copy */
        const uint8_t *src = frame->data[0];
        uint8_t *dst = (uint8_t*)bmdBytes;
        for (int y = 0; y < h; y++) {
            memcpy(dst, src, rowBytes);
            src += frame->linesize[0];
            dst += rowBytes;
        }
    } else {
        /* Color convert & scale via swscale to native UYVY422 */
        if (!ctx->swsCtx || ctx->swsSrcW != frame->width ||
            ctx->swsSrcH != frame->height || ctx->swsSrcFmt != frame->format) {
            if (ctx->swsCtx) sws_freeContext(ctx->swsCtx);
            ctx->swsCtx = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                                         w, h, AV_PIX_FMT_UYVY422,
                                         SWS_FAST_BILINEAR, NULL, NULL, NULL);
            ctx->swsSrcW = frame->width;
            ctx->swsSrcH = frame->height;
            ctx->swsSrcFmt = frame->format;
        }
        if (ctx->swsCtx) {
            uint8_t *dstData[4] = {(uint8_t*)bmdBytes, NULL, NULL, NULL};
            int dstLinesize[4] = {rowBytes, 0, 0, 0};
            sws_scale(ctx->swsCtx, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
        }
    }

    /* Zero-delay synchronous display */
    res = ctx->deckLinkOutput->DisplayVideoFrameSync(bmdFrame);
    bmdFrame->Release();
    ctx->framesWritten++;

    return (res == S_OK) ? 0 : -1;
}

int sdi_output_write_audio_frame(SdiOutputContext *ctx, const AVFrame *frame) {
    if (!ctx || !ctx->deckLinkOutput || !frame || frame->nb_samples <= 0) return -1;
    uint32_t written = 0;
    HRESULT res = ctx->deckLinkOutput->WriteAudioSamplesSync(frame->data[0], frame->nb_samples, &written);
    return (res == S_OK) ? 0 : -1;
}

void sdi_output_close(SdiOutputContext **ctx_ptr) {
    if (!ctx_ptr || !*ctx_ptr) return;
    SdiOutputContext *ctx = *ctx_ptr;

    if (ctx->swsCtx) {
        sws_freeContext(ctx->swsCtx);
        ctx->swsCtx = NULL;
    }
    if (ctx->deckLinkOutput) {
        ctx->deckLinkOutput->DisableVideoOutput();
        ctx->deckLinkOutput->DisableAudioOutput();
        ctx->deckLinkOutput->Release();
    }
    if (ctx->deckLink) {
        ctx->deckLink->Release();
    }

    delete ctx;
    *ctx_ptr = NULL;
    LOG_INFO("[SDI OUT] Playout port closed.");
}
