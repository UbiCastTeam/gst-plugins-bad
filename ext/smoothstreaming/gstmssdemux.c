/* GStreamer
 * Copyright (C) 2012 Smart TV Alliance
 *  Author: Thiago Sousa Santos <thiago.sousa.santos@collabora.com>, Collabora Ltd.
 *
 * gstmssdemux.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-mssdemux
 *
 * Demuxes a Microsoft's Smooth Streaming manifest into its audio and/or video streams.
 *
 *
 */

/*
 * == Internals
 *
 * = Smooth streaming in a few lines
 * A SS stream is defined by a xml manifest file. This file has a list of
 * tracks (StreamIndex), each one can have multiple QualityLevels, that define
 * different encoding/bitrates. When playing a track, only one of those
 * QualityLevels can be active at a time (per stream).
 *
 * The StreamIndex defines a URL with {time} and {bitrate} tags that are
 * replaced by values indicated by the fragment start times and the selected
 * QualityLevel, that generates the fragments URLs.
 *
 * Another relevant detail is that the Isomedia fragments for smoothstreaming
 * won't contains a 'moov' atom, nor a 'stsd', so there is no information
 * about the media type/configuration on the fragments, it must be extracted
 * from the Manifest and passed downstream. mssdemux does this via GstCaps.
 *
 * = How mssdemux works
 * There is a gstmssmanifest.c utility that holds the manifest and parses
 * and has functions to extract information from it. mssdemux received the
 * manifest from its sink pad and starts processing it when it gets EOS.
 *
 * The Manifest is parsed and the streams are exposed, 1 pad for each, with
 * a initially selected QualityLevel. Each stream starts its own GstTaks that
 * is responsible for downloading fragments and pushing them downstream.
 *
 * When a new connection-speed is set, mssdemux evaluates the available
 * QualityLevels and might decide to switch to another one. In this case it
 * pushes a new GstCaps event indicating the new caps on the pads.
 *
 * All operations that intend to update the GstTasks state should be protected
 * with the GST_OBJECT_LOCK.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst/gst-i18n-plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gstmssdemux.h"

GST_DEBUG_CATEGORY (mssdemux_debug);

#define DEFAULT_CONNECTION_SPEED 0
#define DEFAULT_MAX_QUEUE_SIZE_BUFFERS 0
#define DEFAULT_BITRATE_LIMIT 0.8

enum
{
  PROP_0,

  PROP_CONNECTION_SPEED,
  PROP_MAX_QUEUE_SIZE_BUFFERS,
  PROP_BITRATE_LIMIT,
  PROP_LAST
};

static GstStaticPadTemplate gst_mss_demux_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/vnd.ms-sstr+xml")
    );

static GstStaticPadTemplate gst_mss_demux_videosrc_template =
GST_STATIC_PAD_TEMPLATE ("video_%02u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_mss_demux_audiosrc_template =
GST_STATIC_PAD_TEMPLATE ("audio_%02u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

#define gst_mss_demux_parent_class parent_class
G_DEFINE_TYPE (GstMssDemux, gst_mss_demux, GST_TYPE_ADAPTIVE_DEMUX);

static void gst_mss_demux_dispose (GObject * object);
static void gst_mss_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_mss_demux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_mss_demux_is_live (GstAdaptiveDemux * demux);
static gboolean gst_mss_demux_process_manifest (GstAdaptiveDemux * demux,
    GstBuffer * buffer);
static GstClockTime gst_mss_demux_get_duration (GstAdaptiveDemux * demux);
static void gst_mss_demux_reset (GstAdaptiveDemux * demux);
static GstFlowReturn gst_mss_demux_stream_seek (GstAdaptiveDemuxStream * stream,
    GstClockTime ts);
static gboolean
gst_mss_demux_stream_has_next_fragment (GstAdaptiveDemuxStream * stream);
static GstFlowReturn
gst_mss_demux_stream_advance_fragment (GstAdaptiveDemuxStream * stream);
static gboolean gst_mss_demux_stream_select_bitrate (GstAdaptiveDemuxStream *
    stream, guint64 bitrate);
static GstFlowReturn
gst_mss_demux_stream_update_fragment_info (GstAdaptiveDemuxStream * stream);
static gboolean gst_mss_demux_seek (GstAdaptiveDemux * demux, GstEvent * seek);
static gint64
gst_mss_demux_get_manifest_update_interval (GstAdaptiveDemux * demux);
static GstFlowReturn
gst_mss_demux_update_manifest (GstAdaptiveDemux * demux, GstBuffer * buffer);

static void
gst_mss_demux_class_init (GstMssDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstAdaptiveDemuxClass *gstadaptivedemux_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstadaptivedemux_class = (GstAdaptiveDemuxClass *) klass;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_mss_demux_sink_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_mss_demux_videosrc_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_mss_demux_audiosrc_template));
  gst_element_class_set_static_metadata (gstelement_class,
      "Smooth Streaming demuxer", "Codec/Demuxer/Adaptive",
      "Parse and demultiplex a Smooth Streaming manifest into audio and video "
      "streams", "Thiago Santos <thiago.sousa.santos@collabora.com>");

  gobject_class->dispose = gst_mss_demux_dispose;
  gobject_class->set_property = gst_mss_demux_set_property;
  gobject_class->get_property = gst_mss_demux_get_property;

  g_object_class_install_property (gobject_class, PROP_CONNECTION_SPEED,
      g_param_spec_uint ("connection-speed", "Connection Speed",
          "Network connection speed in kbps (0 = unknown)",
          0, G_MAXUINT / 1000, DEFAULT_CONNECTION_SPEED,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

#ifndef GST_REMOVE_DEPRECATED
  g_object_class_install_property (gobject_class, PROP_MAX_QUEUE_SIZE_BUFFERS,
      g_param_spec_uint ("max-queue-size-buffers", "Max queue size in buffers",
          "Maximum buffers that can be stored in each internal stream queue "
          "(0 = infinite) (deprecated)", 0, G_MAXUINT,
          DEFAULT_MAX_QUEUE_SIZE_BUFFERS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_DEPRECATED));
#endif

  g_object_class_install_property (gobject_class, PROP_BITRATE_LIMIT,
      g_param_spec_float ("bitrate-limit",
          "Bitrate limit in %",
          "Limit of the available bitrate to use when switching to alternates.",
          0, 1, DEFAULT_BITRATE_LIMIT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstadaptivedemux_class->process_manifest = gst_mss_demux_process_manifest;
  gstadaptivedemux_class->is_live = gst_mss_demux_is_live;
  gstadaptivedemux_class->get_duration = gst_mss_demux_get_duration;
  gstadaptivedemux_class->get_manifest_update_interval =
      gst_mss_demux_get_manifest_update_interval;
  gstadaptivedemux_class->reset = gst_mss_demux_reset;
  gstadaptivedemux_class->seek = gst_mss_demux_seek;
  gstadaptivedemux_class->stream_seek = gst_mss_demux_stream_seek;
  gstadaptivedemux_class->stream_advance_fragment =
      gst_mss_demux_stream_advance_fragment;
  gstadaptivedemux_class->stream_has_next_fragment =
      gst_mss_demux_stream_has_next_fragment;
  gstadaptivedemux_class->stream_select_bitrate =
      gst_mss_demux_stream_select_bitrate;
  gstadaptivedemux_class->stream_update_fragment_info =
      gst_mss_demux_stream_update_fragment_info;
  gstadaptivedemux_class->update_manifest = gst_mss_demux_update_manifest;

  GST_DEBUG_CATEGORY_INIT (mssdemux_debug, "mssdemux", 0, "mssdemux plugin");
}

static void
gst_mss_demux_init (GstMssDemux * mssdemux)
{
  mssdemux->data_queue_max_size = DEFAULT_MAX_QUEUE_SIZE_BUFFERS;
  mssdemux->bitrate_limit = DEFAULT_BITRATE_LIMIT;

  gst_adaptive_demux_set_stream_struct_size (GST_ADAPTIVE_DEMUX_CAST (mssdemux),
      sizeof (GstMssDemuxStream));
}

static void
gst_mss_demux_reset (GstAdaptiveDemux * demux)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);

  if (mssdemux->manifest) {
    gst_mss_manifest_free (mssdemux->manifest);
    mssdemux->manifest = NULL;
  }
  g_free (mssdemux->base_url);
  mssdemux->base_url = NULL;

  mssdemux->n_videos = mssdemux->n_audios = 0;

}

static void
gst_mss_demux_dispose (GObject * object)
{
  gst_mss_demux_reset (GST_ADAPTIVE_DEMUX_CAST (object));

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_mss_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX (object);

  switch (prop_id) {
    case PROP_CONNECTION_SPEED:
      GST_OBJECT_LOCK (mssdemux);
      mssdemux->connection_speed = g_value_get_uint (value) * 1000;
      mssdemux->update_bitrates = TRUE;
      GST_DEBUG_OBJECT (mssdemux, "Connection speed set to %" G_GUINT64_FORMAT,
          mssdemux->connection_speed);
      GST_OBJECT_UNLOCK (mssdemux);
      break;
    case PROP_MAX_QUEUE_SIZE_BUFFERS:
      mssdemux->data_queue_max_size = g_value_get_uint (value);
      break;
    case PROP_BITRATE_LIMIT:
      mssdemux->bitrate_limit = g_value_get_float (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_mss_demux_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX (object);

  switch (prop_id) {
    case PROP_CONNECTION_SPEED:
      g_value_set_uint (value, mssdemux->connection_speed / 1000);
      break;
    case PROP_MAX_QUEUE_SIZE_BUFFERS:
      g_value_set_uint (value, mssdemux->data_queue_max_size);
      break;
    case PROP_BITRATE_LIMIT:
      g_value_set_float (value, mssdemux->bitrate_limit);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_mss_demux_is_live (GstAdaptiveDemux * demux)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);

  g_return_val_if_fail (mssdemux->manifest != NULL, FALSE);

  return gst_mss_manifest_is_live (mssdemux->manifest);
}

static GstClockTime
gst_mss_demux_get_duration (GstAdaptiveDemux * demux)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);

  g_return_val_if_fail (mssdemux->manifest != NULL, FALSE);

  return gst_mss_manifest_get_gst_duration (mssdemux->manifest);
}

static GstFlowReturn
gst_mss_demux_stream_update_fragment_info (GstAdaptiveDemuxStream * stream)
{
  GstMssDemuxStream *mssstream = (GstMssDemuxStream *) stream;
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (stream->demux);
  GstFlowReturn ret;
  gchar *path = NULL;

  g_free (stream->fragment.uri);
  stream->fragment.uri = NULL;

  ret = gst_mss_stream_get_fragment_url (mssstream->manifest_stream, &path);

  if (ret == GST_FLOW_OK) {
    stream->fragment.uri = g_strdup_printf ("%s/%s", mssdemux->base_url, path);
    stream->fragment.timestamp =
        gst_mss_stream_get_fragment_gst_timestamp (mssstream->manifest_stream);
    stream->fragment.duration =
        gst_mss_stream_get_fragment_gst_duration (mssstream->manifest_stream);
  }
  g_free (path);

  return ret;
}

static GstFlowReturn
gst_mss_demux_stream_seek (GstAdaptiveDemuxStream * stream, GstClockTime ts)
{
  GstMssDemuxStream *mssstream = (GstMssDemuxStream *) stream;

  gst_mss_stream_seek (mssstream->manifest_stream, ts);
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_mss_demux_stream_advance_fragment (GstAdaptiveDemuxStream * stream)
{
  GstMssDemuxStream *mssstream = (GstMssDemuxStream *) stream;

  if (stream->demux->segment.rate >= 0)
    return gst_mss_stream_advance_fragment (mssstream->manifest_stream);
  else
    return gst_mss_stream_regress_fragment (mssstream->manifest_stream);
}

static GstCaps *
create_mss_caps (GstMssDemuxStream * stream, GstCaps * caps)
{
  return gst_caps_new_simple ("video/quicktime", "variant", G_TYPE_STRING,
      "mss-fragmented", "timescale", G_TYPE_UINT64,
      gst_mss_stream_get_timescale (stream->manifest_stream), "media-caps",
      GST_TYPE_CAPS, caps, NULL);
}

static GstPad *
_create_pad (GstMssDemux * mssdemux, GstMssStream * manifeststream)
{
  gchar *name = NULL;
  GstPad *srcpad = NULL;
  GstMssStreamType streamtype;
  GstPadTemplate *tmpl = NULL;

  streamtype = gst_mss_stream_get_type (manifeststream);
  GST_DEBUG_OBJECT (mssdemux, "Found stream of type: %s",
      gst_mss_stream_type_name (streamtype));

  /* TODO use stream's name/bitrate/index as the pad name? */
  if (streamtype == MSS_STREAM_TYPE_VIDEO) {
    name = g_strdup_printf ("video_%02u", mssdemux->n_videos++);
    tmpl = gst_static_pad_template_get (&gst_mss_demux_videosrc_template);
  } else if (streamtype == MSS_STREAM_TYPE_AUDIO) {
    name = g_strdup_printf ("audio_%02u", mssdemux->n_audios++);
    tmpl = gst_static_pad_template_get (&gst_mss_demux_audiosrc_template);
  }

  if (tmpl != NULL) {
    srcpad =
        GST_PAD_CAST (gst_ghost_pad_new_no_target_from_template (name, tmpl));
    g_free (name);
    gst_object_unref (tmpl);
  }
  if (!srcpad) {
    GST_WARNING_OBJECT (mssdemux, "Ignoring unknown type stream");
    return NULL;
  }

  return srcpad;
}

static gboolean
gst_mss_demux_setup_streams (GstAdaptiveDemux * demux)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);
  GSList *streams = gst_mss_manifest_get_streams (mssdemux->manifest);
  GSList *iter;

  if (streams == NULL) {
    GST_INFO_OBJECT (mssdemux, "No streams found in the manifest");
    GST_ELEMENT_ERROR (mssdemux, STREAM, DEMUX,
        (_("This file contains no playable streams.")),
        ("no streams found at the Manifest"));
    return FALSE;
  }

  GST_INFO_OBJECT (mssdemux, "Changing max bitrate to %" G_GUINT64_FORMAT,
      mssdemux->connection_speed);
  gst_mss_manifest_change_bitrate (mssdemux->manifest,
      mssdemux->connection_speed);
  mssdemux->update_bitrates = FALSE;

  for (iter = streams; iter; iter = g_slist_next (iter)) {
    GstPad *srcpad = NULL;
    GstMssDemuxStream *stream = NULL;
    GstMssStream *manifeststream = iter->data;
    GstCaps *caps;
    const gchar *lang;

    srcpad = _create_pad (mssdemux, manifeststream);

    if (!srcpad) {
      continue;
    }

    stream = (GstMssDemuxStream *)
        gst_adaptive_demux_stream_new (GST_ADAPTIVE_DEMUX_CAST (mssdemux),
        srcpad);
    stream->manifest_stream = manifeststream;
    gst_mss_stream_set_active (manifeststream, TRUE);
    caps = gst_mss_stream_get_caps (stream->manifest_stream);
    gst_adaptive_demux_stream_set_caps (GST_ADAPTIVE_DEMUX_STREAM_CAST (stream),
        create_mss_caps (stream, caps));
    gst_caps_unref (caps);

    lang = gst_mss_stream_get_lang (stream->manifest_stream);
    if (lang != NULL) {
      GstTagList *tags;

      tags = gst_tag_list_new (GST_TAG_LANGUAGE_CODE, lang, NULL);
      gst_adaptive_demux_stream_set_tags (GST_ADAPTIVE_DEMUX_STREAM_CAST
          (stream), tags);
    }
  }

  return TRUE;
}

static void
gst_mss_demux_update_base_url (GstMssDemux * mssdemux)
{
  GstAdaptiveDemux *demux = GST_ADAPTIVE_DEMUX_CAST (mssdemux);
  gchar *baseurl_end;

  g_free (mssdemux->base_url);

  mssdemux->base_url =
      g_strdup (demux->manifest_base_uri ? demux->manifest_base_uri : demux->
      manifest_uri);
  baseurl_end = g_strrstr (mssdemux->base_url, "/Manifest");
  if (baseurl_end == NULL) {
    /* second try */
    baseurl_end = g_strrstr (mssdemux->base_url, "/manifest");
  }
  if (baseurl_end) {
    /* set the new end of the string */
    baseurl_end[0] = '\0';
  } else {
    GST_WARNING_OBJECT (mssdemux, "Stream's URI didn't end with /manifest");
  }

}

static gboolean
gst_mss_demux_process_manifest (GstAdaptiveDemux * demux, GstBuffer * buf)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);

  gst_mss_demux_update_base_url (mssdemux);

  mssdemux->manifest = gst_mss_manifest_new (buf);
  if (!mssdemux->manifest) {
    GST_ELEMENT_ERROR (mssdemux, STREAM, FORMAT, ("Bad manifest file"),
        ("Xml manifest file couldn't be parsed"));
    return FALSE;
  }
  return gst_mss_demux_setup_streams (demux);
}

static gboolean
gst_mss_demux_stream_select_bitrate (GstAdaptiveDemuxStream * stream,
    guint64 bitrate)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (stream->demux);
  GstMssDemuxStream *mssstream = (GstMssDemuxStream *) stream;
  gboolean ret = FALSE;

  bitrate *= mssdemux->bitrate_limit;
  if (mssdemux->connection_speed) {
    bitrate = MIN (mssdemux->connection_speed, bitrate);
  }

  GST_DEBUG_OBJECT (stream->pad,
      "Using stream download bitrate %" G_GUINT64_FORMAT, bitrate);

  if (gst_mss_stream_select_bitrate (mssstream->manifest_stream, bitrate)) {
    GstCaps *caps;
    GstCaps *msscaps;
    caps = gst_mss_stream_get_caps (mssstream->manifest_stream);

    GST_DEBUG_OBJECT (stream->pad,
        "Starting streams reconfiguration due to bitrate changes");
    msscaps = create_mss_caps (mssstream, caps);

    GST_DEBUG_OBJECT (stream->pad,
        "Stream changed bitrate to %" G_GUINT64_FORMAT " caps: %"
        GST_PTR_FORMAT,
        gst_mss_stream_get_current_bitrate (mssstream->manifest_stream), caps);

    gst_caps_unref (caps);

    gst_adaptive_demux_stream_set_caps (stream, msscaps);
    ret = TRUE;
    GST_DEBUG_OBJECT (stream->pad, "Finished streams reconfiguration");
  }
  return ret;
}

static gboolean
gst_mss_demux_seek (GstAdaptiveDemux * demux, GstEvent * seek)
{
  gdouble rate;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);

  gst_event_parse_seek (seek, &rate, &format, &flags, &start_type, &start,
      &stop_type, &stop);

  GST_DEBUG_OBJECT (mssdemux,
      "seek event, rate: %f start: %" GST_TIME_FORMAT " stop: %"
      GST_TIME_FORMAT, rate, GST_TIME_ARGS (start), GST_TIME_ARGS (stop));

  gst_mss_manifest_seek (mssdemux->manifest, start);

  return TRUE;
}

static gboolean
gst_mss_demux_stream_has_next_fragment (GstAdaptiveDemuxStream * stream)
{
  GstMssDemuxStream *mssstream = (GstMssDemuxStream *) stream;

  return gst_mss_stream_has_next_fragment (mssstream->manifest_stream);
}

static gint64
gst_mss_demux_get_manifest_update_interval (GstAdaptiveDemux * demux)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);
  GstClockTime interval;

  /* Not much information about this in the MSS spec. It seems that
   * the fragments contain an UUID box that should tell the next
   * fragments time and duration so one wouldn't need to fetch
   * the Manifest again, but we need a fallback here. So use 2 times
   * the current fragment duration */

  interval = gst_mss_manifest_get_min_fragment_duration (mssdemux->manifest);
  if (!GST_CLOCK_TIME_IS_VALID (interval))
    interval = 2 * GST_SECOND;  /* default to 2 seconds */

  interval = 2 * (interval / GST_USECOND);

  return interval;
}

static GstFlowReturn
gst_mss_demux_update_manifest (GstAdaptiveDemux * demux, GstBuffer * buffer)
{
  GstMssDemux *mssdemux = GST_MSS_DEMUX_CAST (demux);

  gst_mss_demux_update_base_url (mssdemux);

  gst_mss_manifest_reload_fragments (mssdemux->manifest, buffer);
  return GST_FLOW_OK;
}
