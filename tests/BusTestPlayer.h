#pragma once

#include "KaraokePlayer.h"
#include <gst/gst.h>

// Exercises the real bus -> handleMessage -> fail path without an audio device.
class BusTestPlayer : public KaraokePlayer {
public:
    BusTestPlayer() : KaraokePlayer(nullptr, "fakesink") {}

    bool postError()
    {
        GError* error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
                                           "Synthetic playback failure");
        GstMessage* message = gst_message_new_error(GST_OBJECT(pipeline()), error,
                                                    "Bus regression test");
        g_error_free(error);
        return gst_element_post_message(pipeline(), message);
    }

    bool waitForFailedPreroll()
    {
        // Wait on GStreamer's streaming thread, without running Qt's tick timer
        // or consuming any bus messages. FAILURE means pre-roll posted an error.
        const auto result = gst_element_get_state(pipeline(), nullptr, nullptr, 3 * GST_SECOND);
        GstBus* bus = gst_element_get_bus(pipeline());
        const bool pending = gst_bus_have_pending(bus);
        gst_object_unref(bus);
        return result == GST_STATE_CHANGE_FAILURE && pending;
    }
};
