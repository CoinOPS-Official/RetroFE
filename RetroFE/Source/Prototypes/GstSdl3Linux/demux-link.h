#pragma once
#include <gst/gst.h>
#include <iostream>
#include <stdexcept>

// qtdemux removes/recreates its source pads across READY cycles. Keep a
// pad-added handler for every generation instead of relying on parse-launch's
// initial delayed link. Audio pads must not be linked to the H.264 parser.
inline void connectVideoDemux(GstElement* pipeline) {
    auto* demux = gst_bin_get_by_name(GST_BIN(pipeline), "demux");
    auto* parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser");
    if (!demux || !parser) {
        if (demux) gst_object_unref(demux);
        if (parser) gst_object_unref(parser);
        throw std::runtime_error("prototype demux/parser missing");
    }
    g_signal_connect_object(demux, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer data) {
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        bool h264 = caps && !gst_caps_is_empty(caps) && !gst_caps_is_any(caps) &&
            gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-h264");
        if (caps) gst_caps_unref(caps);
        if (!h264) return;
        auto* target = gst_element_get_static_pad(GST_ELEMENT(data), "sink");
        if (target && !gst_pad_is_linked(target)) {
            const auto result = gst_pad_link(pad, target);
            std::cout << "Demux H.264 pad link: " << gst_pad_link_get_name(result) << std::endl;
        }
        if (target) gst_object_unref(target);
    }), G_OBJECT(parser), static_cast<GConnectFlags>(0));
    gst_object_unref(parser);
    gst_object_unref(demux);
}
