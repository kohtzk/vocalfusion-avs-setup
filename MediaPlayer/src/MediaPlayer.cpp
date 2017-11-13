/*
 * MediaPlayer.cpp
 *
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <cmath>
#include <cstring>

#include <AVSCommon/AVS/Attachment/AttachmentReader.h>
#include <AVSCommon/AVS/SpeakerConstants/SpeakerConstants.h>
#include <AVSCommon/Utils/Logger/Logger.h>
#include <AVSCommon/Utils/Memory/Memory.h>
#include <PlaylistParser/PlaylistParser.h>

#include "MediaPlayer/AttachmentReaderSource.h"
#include "MediaPlayer/ErrorTypeConversion.h"
#include "MediaPlayer/IStreamSource.h"
#include "MediaPlayer/Normalizer.h"
#include "MediaPlayer/UrlSource.h"

#include "MediaPlayer/MediaPlayer.h"

namespace alexaClientSDK {
namespace mediaPlayer {

using namespace avsCommon::avs::attachment;
using namespace avsCommon::avs::speakerConstants;
using namespace avsCommon::sdkInterfaces;
using namespace avsCommon::utils;
using namespace avsCommon::utils::mediaPlayer;
using namespace avsCommon::utils::memory;

/// String to identify log entries originating from this file.
static const std::string TAG("MediaPlayer");

/// A counter used to increment the source id when a new source is set.
static MediaPlayer::SourceId g_id{0};

/// A link to @c MediaPlayerInterface::ERROR.
static const MediaPlayer::SourceId ERROR_SOURCE_ID = MediaPlayer::ERROR;

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

/// Timeout value for calls to @c gst_element_get_state() calls.
static const unsigned int TIMEOUT_ZERO_NANOSECONDS(0);

/// GStreamer Volume Element Minimum.
static const int8_t GST_SET_VOLUME_MIN = 0;

/// GStreamer Volume Element Maximum.
static const int8_t GST_SET_VOLUME_MAX = 1;

/// GStreamer Volume Adjust Minimum.
static const int8_t GST_ADJUST_VOLUME_MIN = -1;

/// GStreamer Volume Adjust Maximum.
static const int8_t GST_ADJUST_VOLUME_MAX = 1;

/**
 * Processes tags found in the tagList.
 * Called through gst_tag_list_foreach.
 *
 * @param tagList List of tags to iterate over.
 * @param tag A specific tag from the tag list.
 * @param pointerToMutableVectorOfTags Pointer to VectorOfTags. Use push_back to preserve order.
 *
 */
static void collectOneTag(const GstTagList* tagList, const gchar* tag, gpointer pointerToMutableVectorOfTags) {
    auto vectorOfTags = static_cast<VectorOfTags*>(pointerToMutableVectorOfTags);
    int num = gst_tag_list_get_tag_size(tagList, tag);
    for (int index = 0; index < num; ++index) {
        const GValue* val = gst_tag_list_get_value_index(tagList, tag, index);
        MediaPlayerObserverInterface::TagKeyValueType tagKeyValueType;
        tagKeyValueType.key = std::string(tag);
        if (G_VALUE_HOLDS_STRING(val)) {
            tagKeyValueType.value = std::string(g_value_get_string(val));
            tagKeyValueType.type = MediaPlayerObserverInterface::TagType::STRING;
        } else if (G_VALUE_HOLDS_UINT(val)) {
            tagKeyValueType.value = std::to_string(g_value_get_uint(val));
            tagKeyValueType.type = MediaPlayerObserverInterface::TagType::UINT;
        } else if (G_VALUE_HOLDS_INT(val)) {
            tagKeyValueType.value = std::to_string(g_value_get_int(val));
            tagKeyValueType.type = MediaPlayerObserverInterface::TagType::INT;
        } else if (G_VALUE_HOLDS_BOOLEAN(val)) {
            tagKeyValueType.value = std::string(g_value_get_boolean(val) ? "true" : "false");
            tagKeyValueType.type = MediaPlayerObserverInterface::TagType::BOOLEAN;
        } else if (GST_VALUE_HOLDS_DATE_TIME(val)) {
            GstDateTime* dt = static_cast<GstDateTime*>(g_value_get_boxed(val));
            gchar* dt_str = gst_date_time_to_iso8601_string(dt);
            if (!dt_str) {
                continue;
            }
            tagKeyValueType.value = std::string(dt_str);
            tagKeyValueType.type = MediaPlayerObserverInterface::TagType::STRING;
            g_free(dt_str);
        } else if (G_VALUE_HOLDS_DOUBLE(val)) {
            tagKeyValueType.value = std::to_string(g_value_get_double(val));
            tagKeyValueType.type = MediaPlayerObserverInterface::TagType::DOUBLE;
        } else {
            /*
             * Ignore GST_VALUE_HOLDS_BUFFER and other types.
             */
            continue;
        }
        vectorOfTags->push_back(tagKeyValueType);
    }
}

std::shared_ptr<MediaPlayer> MediaPlayer::create(
    std::shared_ptr<avsCommon::sdkInterfaces::HTTPContentFetcherInterfaceFactoryInterface> contentFetcherFactory,
    SpeakerInterface::Type type) {
    ACSDK_DEBUG9(LX("createCalled"));
    std::shared_ptr<MediaPlayer> mediaPlayer(new MediaPlayer(contentFetcherFactory, type));
    if (mediaPlayer->init()) {
        return mediaPlayer;
    } else {
        return nullptr;
    }
};

MediaPlayer::~MediaPlayer() {
    ACSDK_DEBUG9(LX("~MediaPlayerCalled"));
    gst_element_set_state(m_pipeline.pipeline, GST_STATE_NULL);
    if (m_source) {
        m_source->shutdown();
    }
    m_source.reset();
    // Destroy before g_main_loop.
    if (m_setSourceThread.joinable()) {
        m_setSourceThread.join();
    }
    g_main_loop_quit(m_mainLoop);
    if (m_mainLoopThread.joinable()) {
        m_mainLoopThread.join();
    }
    gst_object_unref(m_pipeline.pipeline);
    resetPipeline();

    g_source_remove(m_busWatchId);
    g_main_loop_unref(m_mainLoop);
}

MediaPlayer::SourceId MediaPlayer::setSource(std::shared_ptr<avsCommon::avs::attachment::AttachmentReader> reader) {
    ACSDK_DEBUG9(LX("setSourceCalled").d("sourceType", "AttachmentReader"));
    std::promise<MediaPlayer::SourceId> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, &reader, &promise]() {
        handleSetAttachmentReaderSource(std::move(reader), &promise);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

MediaPlayer::SourceId MediaPlayer::setSource(std::shared_ptr<std::istream> stream, bool repeat) {
    ACSDK_DEBUG9(LX("setSourceCalled").d("sourceType", "istream"));
    std::promise<MediaPlayer::SourceId> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, &stream, repeat, &promise]() {
        handleSetIStreamSource(stream, repeat, &promise);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

MediaPlayer::SourceId MediaPlayer::setSource(const std::string& url) {
    ACSDK_DEBUG9(LX("setSourceForUrlCalled").sensitive("url", url));
    std::promise<MediaPlayer::SourceId> promise;
    auto future = promise.get_future();

    if (m_setSourceThread.joinable()) {
        m_setSourceThread.join();
    }

    std::function<gboolean()> callback = [this, url, &promise]() {

        /*
         * We call the tearDown here instead of inside MediaPlayer::handleSetSource to ensure
         * serialization of the tearDowns in the g_main_loop
         */
        tearDownTransientPipelineElements();

        /*
         * A separate thread is needed because the UrlSource needs block and wait for callbacks
         * from the main event loop (g_main_loop). Deadlock will occur if UrlSource is created
         * on the main event loop.
         *
         * TODO: This thread is only needed for the Totem PlaylistParser.  Need to investigate if
         * it's still needed after a new PlaylistParser is added.
         */
        m_setSourceThread = std::thread(&MediaPlayer::handleSetSource, this, url, &promise);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

bool MediaPlayer::play(MediaPlayer::SourceId id) {
    ACSDK_DEBUG9(LX("playCalled"));
    if (!m_source) {
        ACSDK_ERROR(LX("playFailed").d("reason", "sourceNotSet"));
        return ERROR;
    }

    m_source->preprocess();

    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, id, &promise]() {
        handlePlay(id, &promise);
        return false;
    };

    queueCallback(&callback);
    return future.get();
}

bool MediaPlayer::stop(MediaPlayer::SourceId id) {
    ACSDK_DEBUG9(LX("stopCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, id, &promise]() {
        handleStop(id, &promise);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

bool MediaPlayer::pause(MediaPlayer::SourceId id) {
    ACSDK_DEBUG9(LX("pausedCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, id, &promise]() {
        handlePause(id, &promise);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

bool MediaPlayer::resume(MediaPlayer::SourceId id) {
    ACSDK_DEBUG9(LX("resumeCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, id, &promise]() {
        handleResume(id, &promise);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

std::chrono::milliseconds MediaPlayer::getOffset(MediaPlayer::SourceId id) {
    ACSDK_DEBUG9(LX("getOffsetCalled"));
    std::promise<std::chrono::milliseconds> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, id, &promise]() {
        handleGetOffset(id, &promise);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

bool MediaPlayer::setOffset(MediaPlayer::SourceId id, std::chrono::milliseconds offset) {
    ACSDK_DEBUG9(LX("setOffsetCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, id, &promise, offset]() {
        handleSetOffset(id, &promise, offset);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

void MediaPlayer::setObserver(std::shared_ptr<MediaPlayerObserverInterface> observer) {
    ACSDK_DEBUG9(LX("setObserverCalled"));
    std::promise<void> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, &promise, &observer]() {
        handleSetObserver(&promise, observer);
        return false;
    };
    queueCallback(&callback);
    future.wait();
}

bool MediaPlayer::setVolume(int8_t volume) {
    ACSDK_DEBUG9(LX("setVolumeCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, &promise, volume]() {
        handleSetVolume(&promise, volume);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

void MediaPlayer::handleSetVolume(std::promise<bool>* promise, int8_t volume) {
    ACSDK_DEBUG9(LX("handleSetVolumeCalled"));
    auto toGstVolume =
        Normalizer::create(AVS_SET_VOLUME_MIN, AVS_SET_VOLUME_MAX, GST_SET_VOLUME_MIN, GST_SET_VOLUME_MAX);
    if (!toGstVolume) {
        ACSDK_ERROR(LX("handleSetVolumeFailed").d("reason", "createNormalizerFailed"));
        promise->set_value(false);
        return;
    }

    gdouble gstVolume;
    if (!m_pipeline.volume) {
        ACSDK_ERROR(LX("handleSetVolumeFailed").d("reason", "volumeElementNull"));
        promise->set_value(false);
        return;
    }

    if (!toGstVolume->normalize(volume, &gstVolume)) {
        ACSDK_ERROR(LX("handleSetVolumeFailed").d("reason", "normalizeVolumeFailed"));
        promise->set_value(false);
        return;
    }

    g_object_set(m_pipeline.volume, "volume", gstVolume, NULL);
    promise->set_value(true);
}

bool MediaPlayer::adjustVolume(int8_t delta) {
    ACSDK_DEBUG9(LX("adjustVolumeCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, &promise, delta]() {
        handleAdjustVolume(&promise, delta);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

void MediaPlayer::handleAdjustVolume(std::promise<bool>* promise, int8_t delta) {
    ACSDK_DEBUG9(LX("handleAdjustVolumeCalled"));
    auto toGstDeltaVolume =
        Normalizer::create(AVS_ADJUST_VOLUME_MIN, AVS_ADJUST_VOLUME_MAX, GST_ADJUST_VOLUME_MIN, GST_ADJUST_VOLUME_MAX);

    if (!toGstDeltaVolume) {
        ACSDK_ERROR(LX("handleAdjustVolumeFailed").d("reason", "createNormalizerFailed"));
        promise->set_value(false);
        return;
    }

    if (!m_pipeline.volume) {
        ACSDK_ERROR(LX("adjustVolumeFailed").d("reason", "volumeElementNull"));
        promise->set_value(false);
        return;
    }

    gdouble gstVolume;
    g_object_get(m_pipeline.volume, "volume", &gstVolume, NULL);

    gdouble gstDelta;
    if (!toGstDeltaVolume->normalize(delta, &gstDelta)) {
        ACSDK_ERROR(LX("adjustVolumeFailed").d("reason", "normalizeVolumeFailed"));
        promise->set_value(false);
        return;
    }

    gstVolume += gstDelta;

    // If adjustment exceeds bounds, cap at max/min.
    gstVolume = std::min(gstVolume, static_cast<gdouble>(GST_SET_VOLUME_MAX));
    gstVolume = std::max(gstVolume, static_cast<gdouble>(GST_SET_VOLUME_MIN));

    g_object_set(m_pipeline.volume, "volume", gstVolume, NULL);
    promise->set_value(true);
}

bool MediaPlayer::setMute(bool mute) {
    ACSDK_DEBUG9(LX("setMuteCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, &promise, mute]() {
        handleSetMute(&promise, mute);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

void MediaPlayer::handleSetMute(std::promise<bool>* promise, bool mute) {
    ACSDK_DEBUG9(LX("handleSetMuteCalled"));
    if (!m_pipeline.volume) {
        ACSDK_ERROR(LX("setMuteFailed").d("reason", "volumeElementNull"));
        promise->set_value(false);
        return;
    }

    g_object_set(m_pipeline.volume, "mute", static_cast<gboolean>(mute), NULL);
    promise->set_value(true);
}

bool MediaPlayer::getSpeakerSettings(SpeakerInterface::SpeakerSettings* settings) {
    ACSDK_DEBUG9(LX("getSpeakerSettingsCalled"));
    std::promise<bool> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [this, &promise, settings]() {
        handleGetSpeakerSettings(&promise, settings);
        return false;
    };
    queueCallback(&callback);
    return future.get();
}

void MediaPlayer::handleGetSpeakerSettings(
    std::promise<bool>* promise,
    avsCommon::sdkInterfaces::SpeakerInterface::SpeakerSettings* settings) {
    ACSDK_DEBUG9(LX("handleGetSpeakerSettingsCalled"));
    if (!settings) {
        ACSDK_ERROR(LX("getSpeakerSettingsFailed").d("reason", "nullSettings"));
        promise->set_value(false);
        return;
    } else if (!m_pipeline.volume) {
        ACSDK_ERROR(LX("getSpeakerSettingsFailed").d("reason", "volumeElementNull"));
        promise->set_value(false);
        return;
    }

    auto toAVSVolume =
        Normalizer::create(GST_SET_VOLUME_MIN, GST_SET_VOLUME_MAX, AVS_SET_VOLUME_MIN, AVS_SET_VOLUME_MAX);
    if (!toAVSVolume) {
        ACSDK_ERROR(LX("handleGetSpeakerSettingsFailed").d("reason", "createNormalizerFailed"));
        promise->set_value(false);
        return;
    }

    gdouble avsVolume;
    gdouble gstVolume;
    gboolean mute;
    g_object_get(m_pipeline.volume, "volume", &gstVolume, "mute", &mute, NULL);

    if (!toAVSVolume->normalize(gstVolume, &avsVolume)) {
        ACSDK_ERROR(LX("handleGetSpeakerSettingsFailed").d("reason", "normalizeVolumeFailed"));
        promise->set_value(false);
        return;
    }

    // AVS Volume will be between 0 and 100.
    settings->volume = static_cast<int8_t>(std::round(avsVolume));
    settings->mute = mute;

    promise->set_value(true);
}

SpeakerInterface::Type MediaPlayer::getSpeakerType() {
    ACSDK_DEBUG9(LX("getSpeakerTypeCalled"));
    return m_speakerType;
}

void MediaPlayer::setAppSrc(GstAppSrc* appSrc) {
    m_pipeline.appsrc = appSrc;
}

GstAppSrc* MediaPlayer::getAppSrc() const {
    return m_pipeline.appsrc;
}

void MediaPlayer::setDecoder(GstElement* decoder) {
    m_pipeline.decoder = decoder;
}

GstElement* MediaPlayer::getDecoder() const {
    return m_pipeline.decoder;
}

GstElement* MediaPlayer::getPipeline() const {
    return m_pipeline.pipeline;
}

MediaPlayer::MediaPlayer(
    std::shared_ptr<avsCommon::sdkInterfaces::HTTPContentFetcherInterfaceFactoryInterface> contentFetcherFactory,
    SpeakerInterface::Type type) :
        m_contentFetcherFactory{contentFetcherFactory},
        m_speakerType{type},
        m_playbackStartedSent{false},
        m_playbackFinishedSent{false},
        m_isPaused{false},
        m_isBufferUnderrun{false},
        m_playerObserver{nullptr},
        m_currentId{ERROR},
        m_playPending{false},
        m_pausePending{false},
        m_resumePending{false},
        m_pauseImmediately{false} {
}

bool MediaPlayer::init() {
    if (false == gst_init_check(NULL, NULL, NULL)) {
        ACSDK_ERROR(LX("initPlayerFailed").d("reason", "gstInitCheckFailed"));
        return false;
    }

    if (!(m_mainLoop = g_main_loop_new(nullptr, false))) {
        ACSDK_ERROR(LX("initPlayerFailed").d("reason", "gstMainLoopNewFailed"));
        return false;
    };

    m_mainLoopThread = std::thread(g_main_loop_run, m_mainLoop);

    if (!setupPipeline()) {
        ACSDK_ERROR(LX("initPlayerFailed").d("reason", "setupPipelineFailed"));
        return false;
    }

    return true;
}

bool MediaPlayer::setupPipeline() {
    m_pipeline.converter = gst_element_factory_make("audioconvert", "converter");
    if (!m_pipeline.converter) {
        ACSDK_ERROR(LX("setupPipelineFailed").d("reason", "createConverterElementFailed"));
        return false;
    }

    m_pipeline.volume = gst_element_factory_make("volume", "volume");
    if (!m_pipeline.volume) {
        ACSDK_ERROR(LX("setupPipelineFailed").d("reason", "createVolumeElementFailed"));
        return false;
    }

    m_pipeline.audioSink = gst_element_factory_make("alsasink", "audio_sink");
    if (!m_pipeline.audioSink) {
        ACSDK_ERROR(LX("setupPipelineFailed").d("reason", "createAudioSinkElementFailed"));
        return false;
    }

    m_pipeline.pipeline = gst_pipeline_new("audio-pipeline");
    if (!m_pipeline.pipeline) {
        ACSDK_ERROR(LX("setupPipelineFailed").d("reason", "createPipelineElementFailed"));
        return false;
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.pipeline));
    m_busWatchId = gst_bus_add_watch(bus, &MediaPlayer::onBusMessage, this);
    gst_object_unref(bus);

    // Link only the volume, converter, and sink here. Src will be linked in respective source files.
    gst_bin_add_many(
        GST_BIN(m_pipeline.pipeline), m_pipeline.converter, m_pipeline.volume, m_pipeline.audioSink, nullptr);

    if (!gst_element_link_many(m_pipeline.converter, m_pipeline.volume, m_pipeline.audioSink, nullptr)) {
        ACSDK_ERROR(LX("setupPipelineFailed").d("reason", "createVolumeToConverterToSinkLinkFailed"));
        return false;
    }

    return true;
}

void MediaPlayer::tearDownTransientPipelineElements() {
    ACSDK_DEBUG9(LX("tearDownTransientPipelineElements"));
    if (m_currentId != ERROR_SOURCE_ID) {
        sendPlaybackStopped();
    }
    m_currentId = ERROR_SOURCE_ID;
    if (m_source) {
        m_source->shutdown();
    }
    m_source.reset();
    if (m_pipeline.pipeline) {
        gst_element_set_state(m_pipeline.pipeline, GST_STATE_NULL);
        if (m_pipeline.appsrc) {
            gst_bin_remove(GST_BIN(m_pipeline.pipeline), GST_ELEMENT(m_pipeline.appsrc));
        }
        m_pipeline.appsrc = nullptr;

        if (m_pipeline.decoder) {
            gst_bin_remove(GST_BIN(m_pipeline.pipeline), GST_ELEMENT(m_pipeline.decoder));
        }
        m_pipeline.decoder = nullptr;
    }
    m_offsetManager.clear();
    m_playPending = false;
    m_pausePending = false;
    m_resumePending = false;
    m_pauseImmediately = false;
    m_playbackStartedSent = false;
    m_playbackFinishedSent = false;
    m_isPaused = false;
    m_isBufferUnderrun = false;
}

void MediaPlayer::resetPipeline() {
    ACSDK_DEBUG9(LX("resetPipeline"));
    m_pipeline.pipeline = nullptr;
    m_pipeline.appsrc = nullptr;
    m_pipeline.decoder = nullptr;
    m_pipeline.converter = nullptr;
    m_pipeline.volume = nullptr;
    m_pipeline.audioSink = nullptr;
}

bool MediaPlayer::queryBufferingStatus(bool* buffering) {
    ACSDK_DEBUG9(LX("queryBufferingStatus"));
    GstQuery* query = gst_query_new_buffering(GST_FORMAT_TIME);
    if (gst_element_query(m_pipeline.pipeline, query)) {
        gboolean busy;
        gst_query_parse_buffering_percent(query, &busy, nullptr);
        *buffering = busy;
        ACSDK_INFO(LX("queryBufferingStatus").d("buffering", *buffering));
        gst_query_unref(query);
        return true;
    } else {
        ACSDK_ERROR(LX("queryBufferingStatusFailed").d("reason", "bufferyQueryFailed"));
        gst_query_unref(query);
        return false;
    }
}

bool MediaPlayer::queryIsSeekable(bool* isSeekable) {
    ACSDK_DEBUG9(LX("queryIsSeekable"));
    gboolean seekable;
    GstQuery* query;
    query = gst_query_new_seeking(GST_FORMAT_TIME);
    if (gst_element_query(m_pipeline.pipeline, query)) {
        gst_query_parse_seeking(query, NULL, &seekable, NULL, NULL);
        *isSeekable = (seekable == TRUE);
        ACSDK_DEBUG(LX("queryIsSeekable").d("isSeekable", *isSeekable));
        gst_query_unref(query);
        return true;
    } else {
        ACSDK_ERROR(LX("queryIsSeekableFailed").d("reason", "seekQueryFailed"));
        gst_query_unref(query);
        return false;
    }
}

bool MediaPlayer::seek() {
    bool seekSuccessful = true;
    ACSDK_DEBUG9(LX("seekCalled"));
    if (!m_offsetManager.isSeekable() || !m_offsetManager.isSeekPointSet()) {
        ACSDK_ERROR(LX("seekFailed")
                        .d("reason", "invalidState")
                        .d("isSeekable", m_offsetManager.isSeekable())
                        .d("seekPointSet", m_offsetManager.isSeekPointSet()));
        seekSuccessful = false;
    } else if (!gst_element_seek_simple(
                   m_pipeline.pipeline,
                   GST_FORMAT_TIME,  // ns
                   static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                   std::chrono::duration_cast<std::chrono::nanoseconds>(m_offsetManager.getSeekPoint()).count())) {
        ACSDK_ERROR(LX("seekFailed").d("reason", "gstElementSeekSimpleFailed"));
        seekSuccessful = false;
    } else {
        ACSDK_DEBUG(LX("seekSuccessful").d("offsetInMs", m_offsetManager.getSeekPoint().count()));
    }

    m_offsetManager.clear();
    return seekSuccessful;
}

guint MediaPlayer::queueCallback(const std::function<gboolean()>* callback) {
    return g_idle_add(reinterpret_cast<GSourceFunc>(&onCallback), const_cast<std::function<gboolean()>*>(callback));
}

gboolean MediaPlayer::onCallback(const std::function<gboolean()>* callback) {
    return (*callback)();
}

void MediaPlayer::onPadAdded(GstElement* decoder, GstPad* pad, gpointer pointer) {
    ACSDK_DEBUG9(LX("onPadAddedCalled"));
    auto mediaPlayer = static_cast<MediaPlayer*>(pointer);
    std::promise<void> promise;
    auto future = promise.get_future();
    std::function<gboolean()> callback = [mediaPlayer, &promise, decoder, pad]() {
        mediaPlayer->handlePadAdded(&promise, decoder, pad);
        return false;
    };
    mediaPlayer->queueCallback(&callback);
    future.wait();
}

void MediaPlayer::handlePadAdded(std::promise<void>* promise, GstElement* decoder, GstPad* pad) {
    ACSDK_DEBUG9(LX("handlePadAddedSignalCalled"));
    GstElement* converter = m_pipeline.converter;
    gst_element_link(decoder, converter);
    promise->set_value();
}

gboolean MediaPlayer::onBusMessage(GstBus* bus, GstMessage* message, gpointer mediaPlayer) {
    return static_cast<MediaPlayer*>(mediaPlayer)->handleBusMessage(message);
}

gboolean MediaPlayer::handleBusMessage(GstMessage* message) {
    ACSDK_DEBUG9(LX("messageReceived").d("messageType", gst_message_type_get_name(GST_MESSAGE_TYPE(message))));
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_EOS:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT_CAST(m_pipeline.pipeline)) {
                if (!m_source->handleEndOfStream()) {
                    alexaClientSDK::avsCommon::utils::logger::LogEntry* errorDescription =
                        &(LX("handleBusMessageFailed").d("reason", "sourceHandleEndOfStreamFailed"));
                    ACSDK_ERROR(*errorDescription);
                    sendPlaybackError(ErrorType::MEDIA_ERROR_INTERNAL_DEVICE_ERROR, errorDescription->c_str());
                }

                // Continue playback if there is additional data.
                if (m_source->hasAdditionalData()) {
                    if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(m_pipeline.pipeline, GST_STATE_NULL)) {
                        alexaClientSDK::avsCommon::utils::logger::LogEntry* errorDescription =
                            &(LX("continuingPlaybackFailed").d("reason", "setPiplineToNullFailed"));

                        ACSDK_ERROR(*errorDescription);
                        sendPlaybackError(ErrorType::MEDIA_ERROR_INTERNAL_DEVICE_ERROR, errorDescription->c_str());
                    }

                    if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(m_pipeline.pipeline, GST_STATE_PLAYING)) {
                        alexaClientSDK::avsCommon::utils::logger::LogEntry* errorDescription =
                            &(LX("continuingPlaybackFailed").d("reason", "setPiplineToPlayingFailed"));

                        ACSDK_ERROR(*errorDescription);
                        sendPlaybackError(ErrorType::MEDIA_ERROR_INTERNAL_DEVICE_ERROR, errorDescription->c_str());
                    }
                } else {
                    sendPlaybackFinished();
                }
            }
            break;

        case GST_MESSAGE_ERROR: {
            GError* error;
            gchar* debug;
            gst_message_parse_error(message, &error, &debug);

            std::string messageSrcName = GST_MESSAGE_SRC_NAME(message);
            ACSDK_ERROR(LX("handleBusMessageError")
                            .d("source", messageSrcName)
                            .d("error", error->message)
                            .d("debug", debug ? debug : "noInfo"));
            bool isPlaybackRemote = m_source ? m_source->isPlaybackRemote() : false;
            sendPlaybackError(gerrorToErrorType(error, isPlaybackRemote), error->message);
            g_error_free(error);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            // Check that the state change is for the pipeline.
            if (GST_MESSAGE_SRC(message) == GST_OBJECT_CAST(m_pipeline.pipeline)) {
                GstState oldState;
                GstState newState;
                GstState pendingState;
                gst_message_parse_state_changed(message, &oldState, &newState, &pendingState);
                ACSDK_DEBUG9(LX("State Change")
                                 .d("oldState", gst_element_state_get_name(oldState))
                                 .d("newState", gst_element_state_get_name(newState))
                                 .d("pendingState", gst_element_state_get_name(pendingState)));
                if (GST_STATE_PAUSED == newState && m_pauseImmediately) {
                    if (m_playPending) {
                        sendPlaybackStarted();
                    } else if (m_resumePending) {
                        sendPlaybackResumed();
                    }
                    sendPlaybackPaused();
                } else if (newState == GST_STATE_PLAYING) {
                    if (!m_playbackStartedSent) {
                        sendPlaybackStarted();
                    } else {
                        if (m_isBufferUnderrun) {
                            sendBufferRefilled();
                            m_isBufferUnderrun = false;
                        } else if (m_isPaused) {
                            sendPlaybackResumed();
                            m_isPaused = false;
                        }
                    }
                } else if (
                    newState == GST_STATE_PAUSED && oldState == GST_STATE_READY &&
                    pendingState == GST_STATE_VOID_PENDING) {
                    /*
                     * Currently the hls/hlsdemux/hlssink plugins are needed to handle HLS sources.
                     * No BUFFERING message are sent, and instead the pipeline goes
                     * straight into a PAUSED state with the buffer query failing.
                     *
                     * This behavior has also been observed in a small percentage of unit tests.
                     *
                     * For use case of buffer query failing (ie not supporting buffering) or not currently
                     * buffering, start the playback immediately.
                     */
                    bool buffering = false;
                    if (!queryBufferingStatus(&buffering) || !buffering) {
                        gst_element_set_state(m_pipeline.pipeline, GST_STATE_PLAYING);
                    }
                } else if (newState == GST_STATE_PAUSED && oldState == GST_STATE_PLAYING) {
                    if (m_isBufferUnderrun) {
                        sendBufferUnderrun();
                    } else if (!m_isPaused) {
                        sendPlaybackPaused();
                        m_isPaused = true;
                    }
                } else if (newState == GST_STATE_NULL && oldState == GST_STATE_READY) {
                    sendPlaybackStopped();
                }
            }
            break;
        }
        case GST_MESSAGE_BUFFERING: {
            gint bufferPercent = 0;
            gst_message_parse_buffering(message, &bufferPercent);
            ACSDK_DEBUG9(LX("handleBusMessage").d("message", "GST_MESSAGE_BUFFERING").d("percent", bufferPercent));

            if (bufferPercent < 100) {
                if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(m_pipeline.pipeline, GST_STATE_PAUSED)) {
                    std::string error = "pausingOnBufferUnderrunFailed";
                    ACSDK_ERROR(LX(error));
                    sendPlaybackError(ErrorType::MEDIA_ERROR_INTERNAL_DEVICE_ERROR, error);
                    break;
                }
                // Only enter bufferUnderrun after playback has started.
                if (m_playbackStartedSent) {
                    m_isBufferUnderrun = true;
                }
            } else {
                if (m_pauseImmediately) {
                    // To avoid starting to play if a pause() was called immediately after calling a play()
                    break;
                }
                bool isSeekable = false;
                if (queryIsSeekable(&isSeekable)) {
                    m_offsetManager.setIsSeekable(isSeekable);
                }

                ACSDK_DEBUG9(LX("offsetState")
                                 .d("isSeekable", m_offsetManager.isSeekable())
                                 .d("isSeekPointSet", m_offsetManager.isSeekPointSet()));

                if (m_offsetManager.isSeekable() && m_offsetManager.isSeekPointSet()) {
                    seek();
                } else if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(m_pipeline.pipeline, GST_STATE_PLAYING)) {
                    std::string error = "resumingOnBufferRefilledFailed";
                    ACSDK_ERROR(LX(error));
                    sendPlaybackError(ErrorType::MEDIA_ERROR_INTERNAL_DEVICE_ERROR, error);
                }
            }
            break;
        }
        case GST_MESSAGE_TAG: {
            auto vectorOfTags = collectTags(message);
            sendStreamTagsToObserver(std::move(vectorOfTags));
            break;
        }
        default:
            break;
    }
    return true;
}

std::unique_ptr<const VectorOfTags> MediaPlayer::collectTags(GstMessage* message) {
    VectorOfTags vectorOfTags;
    GstTagList* tags = NULL;
    gst_message_parse_tag(message, &tags);
    int num_of_tags = gst_tag_list_n_tags(tags);
    if (!num_of_tags) {
        gst_tag_list_unref(tags);
        return nullptr;
    }
    gst_tag_list_foreach(tags, &collectOneTag, &vectorOfTags);
    gst_tag_list_unref(tags);
    return make_unique<const VectorOfTags>(vectorOfTags);
}

void MediaPlayer::sendStreamTagsToObserver(std::unique_ptr<const VectorOfTags> vectorOfTags) {
    ACSDK_DEBUG(LX("callingOnTags"));
    if (m_playerObserver) {
        m_playerObserver->onTags(m_currentId, std::move(vectorOfTags));
    }
}

void MediaPlayer::handleSetAttachmentReaderSource(
    std::shared_ptr<AttachmentReader> reader,
    std::promise<MediaPlayer::SourceId>* promise) {
    ACSDK_DEBUG(LX("handleSetSourceCalled"));

    tearDownTransientPipelineElements();

    std::shared_ptr<SourceInterface> source = AttachmentReaderSource::create(this, reader);

    if (!source) {
        ACSDK_ERROR(LX("handleSetAttachmentReaderSourceFailed").d("reason", "sourceIsNullptr"));
        promise->set_value(ERROR_SOURCE_ID);
        return;
    }

    /*
     * Once the source pad for the decoder has been added, the decoder emits the pad-added signal. Connect the signal
     * to the callback which performs the linking of the decoder source pad to the converter sink pad.
     */
    if (!g_signal_connect(m_pipeline.decoder, "pad-added", G_CALLBACK(onPadAdded), this)) {
        ACSDK_ERROR(LX("handleSetAttachmentReaderSourceFailed").d("reason", "connectPadAddedSignalFailed"));
        promise->set_value(ERROR_SOURCE_ID);
        return;
    }

    m_source = source;
    m_currentId = ++g_id;
    promise->set_value(m_currentId);
}

void MediaPlayer::handleSetIStreamSource(
    std::shared_ptr<std::istream> stream,
    bool repeat,
    std::promise<MediaPlayer::SourceId>* promise) {
    ACSDK_DEBUG(LX("handleSetSourceCalled"));

    tearDownTransientPipelineElements();

    std::shared_ptr<SourceInterface> source = IStreamSource::create(this, stream, repeat);

    if (!source) {
        ACSDK_ERROR(LX("handleSetIStreamSourceFailed").d("reason", "sourceIsNullptr"));
        promise->set_value(ERROR_SOURCE_ID);
        return;
    }

    /*
     * Once the source pad for the decoder has been added, the decoder emits the pad-added signal. Connect the signal
     * to the callback which performs the linking of the decoder source pad to the converter sink pad.
     */
    if (!g_signal_connect(m_pipeline.decoder, "pad-added", G_CALLBACK(onPadAdded), this)) {
        ACSDK_ERROR(LX("handleSetIStreamSourceFailed").d("reason", "connectPadAddedSignalFailed"));
        promise->set_value(ERROR_SOURCE_ID);
        return;
    }

    m_source = source;
    m_currentId = ++g_id;
    promise->set_value(m_currentId);
}

void MediaPlayer::handleSetSource(std::string url, std::promise<MediaPlayer::SourceId>* promise) {
    ACSDK_DEBUG(LX("handleSetSourceForUrlCalled"));
    std::shared_ptr<SourceInterface> source =
        UrlSource::create(this, alexaClientSDK::playlistParser::PlaylistParser::create(m_contentFetcherFactory), url);
    if (!source) {
        ACSDK_ERROR(LX("handleSetSourceForUrlFailed").d("reason", "sourceIsNullptr"));
        promise->set_value(ERROR_SOURCE_ID);
        return;
    }

    /*
     * This works with audio only sources. This does not work for any source that has more than one stream.
     * The first pad that is added may not be the correct stream (ie may be a video stream), and will fail.
     *
     * Once the source pad for the decoder has been added, the decoder emits the pad-added signal. Connect the signal
     * to the callback which performs the linking of the decoder source pad to the converter sink pad.
     */
    if (!g_signal_connect(m_pipeline.decoder, "pad-added", G_CALLBACK(onPadAdded), this)) {
        ACSDK_ERROR(LX("handleSetSourceForUrlFailed").d("reason", "connectPadAddedSignalFailed"));
        promise->set_value(ERROR_SOURCE_ID);
        return;
    }

    m_source = source;
    m_currentId = ++g_id;
    promise->set_value(m_currentId);
}

void MediaPlayer::handlePlay(SourceId id, std::promise<bool>* promise) {
    ACSDK_DEBUG(LX("handlePlayCalled").d("idPassed", id).d("currentId", (m_currentId)));
    if (!validateSourceAndId(id)) {
        ACSDK_ERROR(LX("handlePlayFailed"));
        promise->set_value(false);
        return;
    }

    GstState curState;
    auto stateChange = gst_element_get_state(m_pipeline.pipeline, &curState, NULL, TIMEOUT_ZERO_NANOSECONDS);
    if (stateChange == GST_STATE_CHANGE_FAILURE) {
        ACSDK_ERROR(LX("handlePlayFailed").d("reason", "gstElementGetStateFailed"));
        promise->set_value(false);
        return;
    }
    if (curState == GST_STATE_PLAYING) {
        ACSDK_DEBUG(LX("handlePlayFailed").d("reason", "alreadyPlaying"));
        promise->set_value(false);
        return;
    }
    if (m_playPending) {
        ACSDK_DEBUG(LX("handlePlayFailed").d("reason", "playCurrentlyPending"));
        promise->set_value(false);
        return;
    }

    m_playbackFinishedSent = false;
    m_playbackStartedSent = false;
    m_playPending = true;
    m_pauseImmediately = false;
    promise->set_value(true);

    gboolean attemptBuffering;
    g_object_get(m_pipeline.decoder, "use-buffering", &attemptBuffering, NULL);
    ACSDK_DEBUG(LX("handlePlay").d("attemptBuffering", attemptBuffering));

    GstState startingState = GST_STATE_PLAYING;
    if (attemptBuffering) {
        /*
         * Set pipeline to PAUSED state to attempt buffering.
         * The pipeline will be set to PLAY in two ways:
         * i) If buffering is supported, then upon receiving buffer percent = 100.
         * ii) If buffering is not supported, then the pipeline will be set to PLAY immediately.
         */
        startingState = GST_STATE_PAUSED;
    }

    stateChange = gst_element_set_state(m_pipeline.pipeline, startingState);
    ACSDK_DEBUG(LX("handlePlay")
                    .d("startingState", gst_element_state_get_name(startingState))
                    .d("stateReturn", gst_element_state_change_return_get_name(stateChange)));

    alexaClientSDK::avsCommon::utils::logger::LogEntry* errorDescription;
    switch (stateChange) {
        case GST_STATE_CHANGE_FAILURE:
            errorDescription = &(LX("handlePlayFailed").d("reason", "gstElementSetStateFailure"));
            ACSDK_ERROR(*errorDescription);
            sendPlaybackError(ErrorType::MEDIA_ERROR_INTERNAL_DEVICE_ERROR, errorDescription->c_str());
            return;
        default:
            // Allow sending callbacks to be handled on the bus message
            return;
    }
}

void MediaPlayer::handleStop(MediaPlayer::SourceId id, std::promise<bool>* promise) {
    ACSDK_DEBUG(LX("handleStopCalled").d("idPassed", id).d("currentId", (m_currentId)));
    if (!validateSourceAndId(id)) {
        ACSDK_ERROR(LX("handleStopFailed"));
        promise->set_value(false);
        return;
    }

    GstState curState;
    GstState pending;
    auto stateChangeRet = gst_element_get_state(m_pipeline.pipeline, &curState, &pending, TIMEOUT_ZERO_NANOSECONDS);
    if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
        ACSDK_ERROR(LX("handleStopFailed").d("reason", "gstElementGetStateFailure"));
        promise->set_value(false);
        return;
    }

    // Only stop if currently not stopped.
    if (curState == GST_STATE_NULL) {
        ACSDK_ERROR(LX("handleStopFailed").d("reason", "alreadyStopped"));
        promise->set_value(false);
        return;
    }

    if (pending == GST_STATE_NULL) {
        ACSDK_ERROR(LX("handleStopFailed").d("reason", "alreadyStopping"));
        promise->set_value(false);
        return;
    }

    stateChangeRet = gst_element_set_state(m_pipeline.pipeline, GST_STATE_NULL);
    if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
        ACSDK_ERROR(LX("handleStopFailed").d("reason", "gstElementSetStateFailure"));
        promise->set_value(false);
    } else {
        /*
         * Based on GStreamer docs, a gst_element_set_state call to change the state to GST_STATE_NULL will never
         * return GST_STATE_CHANGE_ASYNC.
         */
        promise->set_value(true);
        if (m_playPending) {
            sendPlaybackStarted();
        } else if (m_resumePending) {
            sendPlaybackResumed();
        }
        sendPlaybackStopped();
    }
}

void MediaPlayer::handlePause(MediaPlayer::SourceId id, std::promise<bool>* promise) {
    ACSDK_DEBUG(LX("handlePauseCalled").d("idPassed", id).d("currentId", (m_currentId)));
    if (!validateSourceAndId(id)) {
        ACSDK_ERROR(LX("handlePauseFailed"));
        promise->set_value(false);
        return;
    }

    GstState curState;
    auto stateChangeRet = gst_element_get_state(m_pipeline.pipeline, &curState, NULL, TIMEOUT_ZERO_NANOSECONDS);
    if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
        ACSDK_ERROR(LX("handlePauseFailed").d("reason", "gstElementGetStateFailure"));
        promise->set_value(false);
        return;
    }

    /*
     * If a play() or resume() call is pending, we want to try pausing immediately to avoid blips in audio.
     */
    if (m_playPending || m_resumePending) {
        ACSDK_DEBUG9(LX("handlePauseCalled").d("info", "playOrResumePending"));
        if (m_pausePending) {
            ACSDK_DEBUG(LX("handlePauseFailed").d("reason", "pauseCurrentlyPending"));
            promise->set_value(false);
            return;
        }
        stateChangeRet = gst_element_set_state(m_pipeline.pipeline, GST_STATE_PAUSED);
        if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
            ACSDK_ERROR(LX("handlePauseFailed").d("reason", "gstElementSetStateFailure"));
            promise->set_value(false);
        } else {
            m_pauseImmediately = true;
            promise->set_value(true);
        }
        return;
    }

    if (curState != GST_STATE_PLAYING) {
        ACSDK_ERROR(LX("handlePauseFailed").d("reason", "noAudioPlaying"));
        promise->set_value(false);
        return;
    }
    if (m_pausePending) {
        ACSDK_DEBUG(LX("handlePauseFailed").d("reason", "pauseCurrentlyPending"));
        promise->set_value(false);
        return;
    }

    stateChangeRet = gst_element_set_state(m_pipeline.pipeline, GST_STATE_PAUSED);
    if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
        ACSDK_ERROR(LX("handlePauseFailed").d("reason", "gstElementSetStateFailure"));
        promise->set_value(false);
    } else {
        m_pausePending = true;
        promise->set_value(true);
    }
}

void MediaPlayer::handleResume(MediaPlayer::SourceId id, std::promise<bool>* promise) {
    ACSDK_DEBUG(LX("handleResumeCalled").d("idPassed", id).d("currentId", (m_currentId)));
    if (!validateSourceAndId(id)) {
        ACSDK_ERROR(LX("handleResumeFailed"));
        promise->set_value(false);
        return;
    }

    GstState curState;
    auto stateChangeRet = gst_element_get_state(m_pipeline.pipeline, &curState, NULL, TIMEOUT_ZERO_NANOSECONDS);

    if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
        ACSDK_ERROR(LX("handleResumeFailed").d("reason", "gstElementGetStateFailure"));
        promise->set_value(false);
        return;
    }

    if (GST_STATE_PLAYING == curState) {
        ACSDK_ERROR(LX("handleResumeFailed").d("reason", "alreadyPlaying"));
        promise->set_value(false);
        return;
    }

    // Only unpause if currently paused.
    if (GST_STATE_PAUSED != curState) {
        ACSDK_ERROR(LX("handleResumeFailed").d("reason", "notCurrentlyPaused"));
        promise->set_value(false);
        return;
    }

    if (m_resumePending) {
        ACSDK_DEBUG(LX("handleResumeFailed").d("reason", "resumeCurrentlyPending"));
        promise->set_value(false);
        return;
    }

    stateChangeRet = gst_element_set_state(m_pipeline.pipeline, GST_STATE_PLAYING);
    if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
        ACSDK_ERROR(LX("handleResumeFailed").d("reason", "gstElementSetStateFailure"));
        promise->set_value(false);
    } else {
        m_resumePending = true;
        m_pauseImmediately = false;
        promise->set_value(true);
    }
}

void MediaPlayer::handleGetOffset(SourceId id, std::promise<std::chrono::milliseconds>* promise) {
    ACSDK_DEBUG(LX("handleGetOffsetCalled").d("idPassed", id).d("currentId", (m_currentId)));
    gint64 position = -1;
    GstState state;

    // Check if pipeline is set.
    if (!m_pipeline.pipeline) {
        ACSDK_INFO(LX("handleGetOffsetStopped").m("pipelineNotSet"));
        promise->set_value(MEDIA_PLAYER_INVALID_OFFSET);
        return;
    }

    if (!validateSourceAndId(id)) {
        promise->set_value(MEDIA_PLAYER_INVALID_OFFSET);
        return;
    }

    auto stateChangeRet = gst_element_get_state(m_pipeline.pipeline, &state, NULL, TIMEOUT_ZERO_NANOSECONDS);

    if (GST_STATE_CHANGE_FAILURE == stateChangeRet) {
        // Getting the state failed.
        ACSDK_ERROR(LX("handleGetOffsetFailed").d("reason", "getElementGetStateFailure"));
    } else if (GST_STATE_CHANGE_SUCCESS != stateChangeRet) {
        // Getting the state was not successful (GST_STATE_CHANGE_ASYNC or GST_STATE_CHANGE_NO_PREROLL).
        ACSDK_INFO(LX("handleGetOffset")
                       .d("reason", "getElementGetStateUnsuccessful")
                       .d("stateChangeReturn", gst_element_state_change_return_get_name(stateChangeRet)));
    } else if (GST_STATE_PAUSED != state && GST_STATE_PLAYING != state) {
        // Invalid State.
        std::ostringstream expectedStates;
        expectedStates << gst_element_state_get_name(GST_STATE_PAUSED) << "/"
                       << gst_element_state_get_name(GST_STATE_PLAYING);
        ACSDK_ERROR(LX("handleGetOffsetFailed")
                        .d("reason", "invalidPipelineState")
                        .d("state", gst_element_state_get_name(state))
                        .d("expectedStates", expectedStates.str()));
    } else if (!gst_element_query_position(m_pipeline.pipeline, GST_FORMAT_TIME, &position)) {
        // Query Failed.
        ACSDK_ERROR(LX("handleGetOffsetInMillisecondsFailed").d("reason", "gstElementQueryPositionError"));
    } else {
        // Query succeeded.
        promise->set_value(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(position)));
        return;
    }

    promise->set_value(MEDIA_PLAYER_INVALID_OFFSET);
}

void MediaPlayer::handleSetOffset(
    MediaPlayer::SourceId id,
    std::promise<bool>* promise,
    std::chrono::milliseconds offset) {
    ACSDK_DEBUG(LX("handleSetOffsetCalled").d("idPassed", id).d("currentId", (m_currentId)));
    if (!validateSourceAndId(id)) {
        promise->set_value(false);
        return;
    }
    m_offsetManager.setSeekPoint(offset);
    promise->set_value(true);
}

void MediaPlayer::handleSetObserver(
    std::promise<void>* promise,
    std::shared_ptr<avsCommon::utils::mediaPlayer::MediaPlayerObserverInterface> observer) {
    ACSDK_DEBUG(LX("handleSetObserverCalled"));
    m_playerObserver = observer;
    promise->set_value();
}

void MediaPlayer::sendPlaybackStarted() {
    if (!m_playbackStartedSent) {
        ACSDK_DEBUG(LX("callingOnPlaybackStarted").d("currentId", m_currentId));
        m_playbackStartedSent = true;
        m_playPending = false;
        if (m_playerObserver) {
            m_playerObserver->onPlaybackStarted(m_currentId);
        }
    }
}

void MediaPlayer::sendPlaybackFinished() {
    if (m_source) {
        m_source->shutdown();
    }
    m_source.reset();
    m_isPaused = false;
    m_playbackStartedSent = false;
    if (!m_playbackFinishedSent) {
        m_playbackFinishedSent = true;
        ACSDK_DEBUG(LX("callingOnPlaybackFinished").d("currentId", m_currentId));
        if (m_playerObserver) {
            m_playerObserver->onPlaybackFinished(m_currentId);
        }
    }
    m_currentId = ERROR_SOURCE_ID;
    tearDownTransientPipelineElements();
}

void MediaPlayer::sendPlaybackPaused() {
    ACSDK_DEBUG(LX("callingOnPlaybackPaused").d("currentId", m_currentId));
    m_pausePending = false;
    if (m_playerObserver) {
        m_playerObserver->onPlaybackPaused(m_currentId);
    }
}

void MediaPlayer::sendPlaybackResumed() {
    ACSDK_DEBUG(LX("callingOnPlaybackResumed").d("currentId", m_currentId));
    m_resumePending = false;
    if (m_playerObserver) {
        m_playerObserver->onPlaybackResumed(m_currentId);
    }
}

void MediaPlayer::sendPlaybackStopped() {
    ACSDK_DEBUG(LX("callingOnPlaybackStopped").d("currentId", m_currentId));
    if (m_playerObserver && ERROR_SOURCE_ID != m_currentId) {
        m_playerObserver->onPlaybackStopped(m_currentId);
    }
    m_currentId = ERROR_SOURCE_ID;
    tearDownTransientPipelineElements();
}

void MediaPlayer::sendPlaybackError(const ErrorType& type, const std::string& error) {
    ACSDK_DEBUG(LX("callingOnPlaybackError").d("type", type).d("error", error).d("currentId", m_currentId));
    m_playPending = false;
    m_pausePending = false;
    m_resumePending = false;
    m_pauseImmediately = false;
    if (m_playerObserver) {
        m_playerObserver->onPlaybackError(m_currentId, type, error);
    }
    m_currentId = ERROR_SOURCE_ID;
    tearDownTransientPipelineElements();
}

void MediaPlayer::sendBufferUnderrun() {
    ACSDK_DEBUG(LX("callingOnBufferUnderrun").d("currentId", m_currentId));
    if (m_playerObserver) {
        m_playerObserver->onBufferUnderrun(m_currentId);
    }
}

void MediaPlayer::sendBufferRefilled() {
    ACSDK_DEBUG(LX("callingOnBufferRefilled").d("currentId", m_currentId));
    if (m_playerObserver) {
        m_playerObserver->onBufferRefilled(m_currentId);
    }
}

bool MediaPlayer::validateSourceAndId(SourceId id) {
    if (!m_source) {
        ACSDK_ERROR(LX("validateSourceAndIdFailed").d("reason", "sourceNotSet"));
        return false;
    }
    if (id != m_currentId) {
        ACSDK_ERROR(LX("validateSourceAndIdFailed").d("reason", "sourceIdMismatch"));
        return false;
    }
    return true;
}

}  // namespace mediaPlayer
}  // namespace alexaClientSDK
