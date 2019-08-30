/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <binder/ProcessState.h>

#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>

#include <ui/DisplayInfo.h>
#include <ui/PixelFormat.h>
#include <utils/Mutex.h>

#include <system/graphics.h>

#include "screencap.h"
#include "../../mjpg_streamer.h"

// TODO: Fix Skia.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <SkImageEncoder.h>
#include <SkData.h>
#include <SkColorSpace.h>
#pragma GCC diagnostic pop

using namespace android;
using std::queue;

struct encode_buf_info {
    unsigned char* pbuf;
    int size;
};

android::Mutex mLock;
queue<sp<GraphicBuffer>> GBufferQueue;
queue<struct encode_buf_info> EncodedBufferQueue;

static uint32_t DEFAULT_DISPLAY_ID = ISurfaceComposer::eDisplayIdMain;

#define COLORSPACE_UNKNOWN    0
#define COLORSPACE_SRGB       1
#define COLORSPACE_DISPLAY_P3 2

// Maps orientations from DisplayInfo to ISurfaceComposer
static const uint32_t ORIENTATION_MAP[] = {
    ISurfaceComposer::eRotateNone, // 0 == DISPLAY_ORIENTATION_0
    ISurfaceComposer::eRotate270, // 1 == DISPLAY_ORIENTATION_90
    ISurfaceComposer::eRotate180, // 2 == DISPLAY_ORIENTATION_180
    ISurfaceComposer::eRotate90, // 3 == DISPLAY_ORIENTATION_270
};

/*
static void usage(const char* pname)
{
    fprintf(stderr,
            "usage: %s [-hp] [-d display-id] [FILENAME]\n"
            "   -h: this message\n"
            "   -p: save the file as a png.\n"
            "   -j: save the file as a jpeg.\n"
            "   -e: encode file format specified as option.\n"
            "   -d: specify the display id to capture, default %d.\n"
            "If FILENAME is not given, the results will be printed to stdout.\n",
            pname, DEFAULT_DISPLAY_ID
    );
}
*/

static SkColorType flinger2skia(PixelFormat f)
{
    switch (f) {
        case PIXEL_FORMAT_RGB_565:
            return kRGB_565_SkColorType;
        default:
            return kN32_SkColorType;
    }
}

static sk_sp<SkColorSpace> dataSpaceToColorSpace(android_dataspace d)
{
    switch (d) {
        case HAL_DATASPACE_V0_SRGB:
            return SkColorSpace::MakeSRGB();
        case HAL_DATASPACE_DISPLAY_P3:
            return SkColorSpace::MakeRGB(
                    SkColorSpace::kSRGB_RenderTargetGamma, SkColorSpace::kDCIP3_D65_Gamut);
        default:
            return nullptr;
    }
}

// int main(int argc, char** argv)
// buf --- alloc in this function, user must free it after used
// size: output param
// type: input param
// TODO
void* captureScreen_thread(void* args) {
        // setThreadPoolMaxThreadCount(0) actually tells the kernel it's
        // not allowed to spawn any additional threads, but we still spawn
        // a binder thread from userspace when we call startThreadPool().
        // See b/36066697 for rationale
        ProcessState::self()->setThreadPoolMaxThreadCount(0);
        ProcessState::self()->startThreadPool();
    while (1) {
        int32_t displayId = DEFAULT_DISPLAY_ID;

        DBG("captureScreen_thread Enter GBufferQueue size: %zd\n", GBufferQueue.size());

        sp<IBinder> display = SurfaceComposerClient::getBuiltInDisplay(displayId);
        if (display == NULL) {
            fprintf(stderr, "Unable to get handle for display %d\n", displayId);
            // b/36066697: Avoid running static destructors.
            _exit(1);
        }

        Vector<DisplayInfo> configs;
        SurfaceComposerClient::getDisplayConfigs(display, &configs);
        int activeConfig = SurfaceComposerClient::getActiveConfig(display);
        if (static_cast<size_t>(activeConfig) >= configs.size()) {
            fprintf(stderr, "Active config %d not inside configs (size %zu)\n",
                    activeConfig, configs.size());
            // b/36066697: Avoid running static destructors.
            _exit(1);
        }
        uint8_t displayOrientation = configs[activeConfig].orientation;
        uint32_t captureOrientation = ORIENTATION_MAP[displayOrientation];

        while (GBufferQueue.size() > 3) {
            DBG("############### too many GBuffer ready, wait consumer\n");
            usleep(1000*50);
        }

        sp<GraphicBuffer> outBuffer;
        status_t result = ScreenshotClient::capture(display, Rect(), 0 /* reqWidth */,
                0 /* reqHeight */, INT32_MIN, INT32_MAX, /* all layers */ false, captureOrientation,
                &outBuffer);
        if (result != NO_ERROR) {
            // close(fd);
            _exit(1);
        }

        {
            android::Mutex::Autolock _l(mLock);
            GBufferQueue.push(outBuffer);
        }
    }
    return NULL;
}

void* encode_thread(void* args) {
    while (1) {
        void* base = NULL;
        uint32_t w, s, h, f;
        android_dataspace d;
        size_t size = 0;
        sp<GraphicBuffer> outBuffer;
        struct encode_buf_info encode_buf;

        DBG("encode_thread Enter GBufferQueue size: %zd, EncodedBufferQueue size: %zd\n", GBufferQueue.size(), EncodedBufferQueue.size());

        while (GBufferQueue.size() < 1 || EncodedBufferQueue.size() > 6) {
            DBG("######### waiting for capture or client\n");
            usleep(1000*10);
        }

        {
            android::Mutex::Autolock _l(mLock);
            outBuffer = GBufferQueue.front();
        }

        status_t result = outBuffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, &base);

        if (result != NO_ERROR) {
            _exit(1);
        }

        if (base == NULL) {
            _exit(1);
        }

        w = outBuffer->getWidth();
        h = outBuffer->getHeight();
        s = outBuffer->getStride();
        f = outBuffer->getPixelFormat();
        d = HAL_DATASPACE_UNKNOWN;
        size = s * h * bytesPerPixel(f);

        encode_buf.pbuf = (unsigned char*)malloc(16*1024*1024);

        if(encode_buf.pbuf == NULL) {
            fprintf(stderr, "could not allocate memory\n");
            _exit(1);
        }

        const SkImageInfo info =
            SkImageInfo::Make(w, h, flinger2skia(f), kPremul_SkAlphaType, dataSpaceToColorSpace(d));
        SkPixmap pixmap(info, base, s * bytesPerPixel(f));
        struct FDWStream final : public SkWStream {
            size_t fBytesWritten = 0;
            // int fFd;
            unsigned char* mBuf;
            FDWStream(unsigned char* buf) : mBuf(buf) {}
            size_t bytesWritten() const override { return fBytesWritten; }
            bool write(const void * buffer, size_t size) override {
            fBytesWritten += size;
            memcpy(mBuf, buffer, size);
            mBuf+=size;
            return true;
            }
        } fdStream(encode_buf.pbuf);

        (void)SkEncodeImage(&fdStream, pixmap, SkEncodedImageFormat::kJPEG, 100);

        encode_buf.size = fdStream.bytesWritten();

        {
            android::Mutex::Autolock _l(mLock);
            EncodedBufferQueue.push(encode_buf);
            GBufferQueue.pop();
        }
    }


    return 0;
}

// int main(int argc, char** argv)
// buf --- alloc in this function, user must free it after used
// size: output param
// type: input param
// TODO
int getEncodedBuf(unsigned char** in_buf, int* out_size/*format_t type*/) {
    DBG("getEncodedBuf Enter, EncodedBufferQueue size: %zd\n", EncodedBufferQueue.size());

    while (EncodedBufferQueue.size() < 1) {
        DBG("###############        waiting for encoded buffer\n");
        usleep(1000*50);
    }

    struct encode_buf_info info;

    android::Mutex::Autolock _l(mLock);

    info = EncodedBufferQueue.front();

    *in_buf = info.pbuf;
    *out_size = info.size;

    EncodedBufferQueue.pop();

    return 0;
}
