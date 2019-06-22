/*
 * Copyright (C) 2019 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "JpegCompressor"

#include <camera3.h>
#include <utils/Log.h>
#include <utils/Trace.h>

#include "JpegCompressor.h"

namespace android {

using google_camera_hal::ErrorCode;
using google_camera_hal::NotifyMessage;
using google_camera_hal::MessageType;

JpegCompressor::JpegCompressor() {
    ATRACE_CALL();
    mJpegProcessingThread = std::thread([this] { this->threadLoop(); });
}

JpegCompressor::~JpegCompressor() {
    ATRACE_CALL();

    // Abort the ongoing compression and flush any pending jobs
    mJpegDone = true;
    mCondition.notify_one();
    mJpegProcessingThread.join();
    while (!mPendingJobs.empty()) {
        auto job = std::move(mPendingJobs.front());
        job->output->streamBuffer.status = BufferStatus::kError;
        mPendingJobs.pop();
    }
}

status_t JpegCompressor::queue(std::unique_ptr<JpegJob> job) {
    ATRACE_CALL();

    if ((job.get() == nullptr) || (job->result.get() == nullptr)) {
        return BAD_VALUE;
    }

    if ((job->input.get() == nullptr) || (job->output.get() == nullptr) ||
            (job->output->format != HAL_PIXEL_FORMAT_BLOB) ||
            (job->output->dataSpace != HAL_DATASPACE_V0_JFIF)) {
        ALOGE("%s: Unable to find buffers for JPEG source/destination", __FUNCTION__);
        return BAD_VALUE;
    }

    if (!job->result->output_buffers.empty()) {
        ALOGE("%s: Jpeg compressor doesn't not support %zu parallel outputs!", __FUNCTION__,
                job->result->output_buffers.size());
        return BAD_VALUE;
    }

    std::unique_lock<std::mutex> lock(mMutex);
    mPendingJobs.push(std::move(job));
    mCondition.notify_one();

    return OK;
}

void JpegCompressor::threadLoop() {
    ATRACE_CALL();

    while (!mJpegDone) {
        std::unique_ptr<JpegJob> currentJob = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mPendingJobs.empty()) {
                currentJob = std::move(mPendingJobs.front());
                mPendingJobs.pop();
            }
        }

        if (currentJob.get() != nullptr) {
            compress(std::move(currentJob));
        }

        std::unique_lock<std::mutex> lock(mMutex);
        auto ret = mCondition.wait_for(lock, std::chrono::milliseconds(10));
        if (ret == std::cv_status::timeout) {
            ALOGV("%s: Jpeg thread timeout", __FUNCTION__);
        }
    }
}

void JpegCompressor::compress(std::unique_ptr<JpegJob> job) {
    ATRACE_CALL();

    struct CustomJpegDestMgr : public jpeg_destination_mgr {
        JOCTET *buffer;
        size_t bufferSize;
        size_t encodedSize;
        bool success;
    } dmgr;

    // Set up error management
    mJpegErrorInfo = NULL;
    jpeg_error_mgr jerr;

    auto cinfo = std::make_unique<jpeg_compress_struct>();
    cinfo->err = jpeg_std_error(&jerr);
    cinfo->err->error_exit = [](j_common_ptr cinfo) {
        (*cinfo->err->output_message)(cinfo);
        if(cinfo->client_data) {
            auto & dmgr = *static_cast<CustomJpegDestMgr*>(cinfo->client_data);
            dmgr.success = false;
        }
    };

    jpeg_create_compress(cinfo.get());
    if (checkError("Error initializing compression")) {
        job->output->streamBuffer.status = BufferStatus::kError;
        return;
    }

    dmgr.buffer = static_cast<JOCTET*>(job->output->plane.img.img);
    dmgr.bufferSize = job->output->plane.img.bufferSize;
    dmgr.encodedSize = 0;
    dmgr.success = true;
    cinfo->client_data = static_cast<void*>(&dmgr);
    dmgr.init_destination = [](j_compress_ptr cinfo) {
        auto & dmgr = static_cast<CustomJpegDestMgr&>(*cinfo->dest);
        dmgr.next_output_byte = dmgr.buffer;
        dmgr.free_in_buffer = dmgr.bufferSize;
        ALOGV("%s:%d jpeg start: %p [%zu]", __FUNCTION__, __LINE__, dmgr.buffer, dmgr.bufferSize);
    };

    dmgr.empty_output_buffer = [](j_compress_ptr cinfo __unused) {
        ALOGV("%s:%d Out of buffer", __FUNCTION__, __LINE__);
        return 0;
    };

    dmgr.term_destination = [](j_compress_ptr cinfo) {
        auto & dmgr = static_cast<CustomJpegDestMgr&>(*cinfo->dest);
        dmgr.encodedSize = dmgr.bufferSize - dmgr.free_in_buffer;
        ALOGV("%s:%d Done with jpeg: %zu", __FUNCTION__, __LINE__, dmgr.encodedSize);
    };

    cinfo->dest = reinterpret_cast<struct jpeg_destination_mgr*>(&dmgr);

    // Set up compression parameters
    cinfo->image_width = job->input->width;
    cinfo->image_height = job->input->height;
    cinfo->input_components = 3;
    cinfo->in_color_space = JCS_RGB;

    jpeg_set_defaults(cinfo.get());
    if (checkError("Error configuring defaults")) {
        job->output->streamBuffer.status = BufferStatus::kError;
        return;
    }

    // Do compression
    jpeg_start_compress(cinfo.get(), TRUE);
    if (checkError("Error starting compression")) {
        job->output->streamBuffer.status = BufferStatus::kError;
        return;
    }

    size_t rowStride = job->input->stride;
    const size_t kChunkSize = 32;
    while (cinfo->next_scanline < cinfo->image_height) {
        JSAMPROW chunk[kChunkSize];
        for (size_t i = 0; i < kChunkSize; i++) {
            chunk[i] = (JSAMPROW)(job->input->img +
                    (i + cinfo->next_scanline) * rowStride);
        }
        jpeg_write_scanlines(cinfo.get(), chunk, kChunkSize);
        if (checkError("Error while compressing")) {
            job->output->streamBuffer.status = BufferStatus::kError;
            return;
        }

        if (mJpegDone) {
            ALOGV("%s: Cancel called, exiting early", __FUNCTION__);
            jpeg_finish_compress(cinfo.get());
            job->output->streamBuffer.status = BufferStatus::kError;
            return;
        }
    }

    jpeg_finish_compress(cinfo.get());
    if (checkError("Error while finishing compression")) {
        job->output->streamBuffer.status = BufferStatus::kError;
        return;
    }

    auto jpegHeaderOffset = job->output->plane.img.bufferSize - sizeof(struct camera3_jpeg_blob);
    if (jpegHeaderOffset > dmgr.encodedSize) {
        struct camera3_jpeg_blob *blob = reinterpret_cast<struct camera3_jpeg_blob*> (
                job->output->plane.img.img + jpegHeaderOffset);
        blob->jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
        blob->jpeg_size = dmgr.encodedSize;
    } else {
        ALOGW("%s: No space for jpeg header at offset: %u and jpeg size: %u", __FUNCTION__,
                jpegHeaderOffset, dmgr.encodedSize);
    }

    // All done
    job->output->streamBuffer.status = dmgr.success ? BufferStatus::kOk : BufferStatus::kError;
}

bool JpegCompressor::checkError(const char *msg) {
    if (mJpegErrorInfo) {
        char errBuffer[JMSG_LENGTH_MAX];
        mJpegErrorInfo->err->format_message(mJpegErrorInfo, errBuffer);
        ALOGE("%s: %s: %s", __FUNCTION__, msg, errBuffer);
        mJpegErrorInfo = NULL;
        return true;
    }

    return false;
}

}  // namespace android