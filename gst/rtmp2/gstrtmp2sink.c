/* GStreamer
 * Copyright (C) 2014 David Schleef <ds@schleef.org>
 * Copyright (C) 2017 Make.TV, Inc. <info@make.tv>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstrtmp2sink
 *
 * The rtmp2sink element sends audio and video streams to an RTMP
 * server.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! x264enc ! flvmux ! rtmp2sink
 *     location=rtmp://server.example.com/live/myStream
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstrtmp2sink.h"

#include "gstrtmp2locationhandler.h"
#include "rtmp/rtmpclient.h"
#include "rtmp/rtmpmessage.h"
#include "rtmp/rtmputils.h"

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_rtmp2_sink_debug_category);
#define GST_CAT_DEFAULT gst_rtmp2_sink_debug_category

/* prototypes */
#define GST_RTMP2_SINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTMP2_SINK,GstRtmp2Sink))
#define GST_RTMP2_SINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTMP2_SINK,GstRtmp2SinkClass))
#define GST_IS_RTMP2_SINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTMP2_SINK))
#define GST_IS_RTMP2_SINK_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTMP2_SINK)
typedef struct _GstRtmp2Sink GstRtmp2Sink;
typedef struct _GstRtmp2SinkClass GstRtmp2SinkClass;

struct _GstRtmp2Sink
{
  GstBaseSink parent_instance;

  /* properties */
  GstRtmpLocation location;

  /* stuff */
  GMutex lock;
  GCond cond;
  gboolean running, flushing;
  GstTask *task;
  GRecMutex task_lock;
  GMainLoop *task_main_loop;
  GMainContext *task_main_context;
  GPtrArray *headers;

  /* timestamp fixup */
  guint64 last_ts, base_ts;

  GTask *connect_task;
  GstRtmpConnection *connection;
};

struct _GstRtmp2SinkClass
{
  GstBaseSinkClass parent_class;
};

/* GObject virtual functions */
static void gst_rtmp2_sink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_rtmp2_sink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_rtmp2_sink_finalize (GObject * object);
static void gst_rtmp2_sink_uri_handler_init (GstURIHandlerInterface * iface);

/* GstBaseSink virtual functions */
static gboolean gst_rtmp2_sink_start (GstBaseSink * sink);
static gboolean gst_rtmp2_sink_stop (GstBaseSink * sink);
static gboolean gst_rtmp2_sink_unlock (GstBaseSink * sink);
static gboolean gst_rtmp2_sink_unlock_stop (GstBaseSink * sink);
static GstFlowReturn gst_rtmp2_sink_render (GstBaseSink * sink,
    GstBuffer * buffer);
static gboolean gst_rtmp2_sink_set_caps (GstBaseSink * sink, GstCaps * caps);

/* Internal API */
static void gst_rtmp2_sink_task (gpointer user_data);
static void client_connect_done (GObject * source, GAsyncResult * result,
    gpointer user_data);
static void send_create_stream (GTask * task);
static void create_stream_done (const gchar * command_name, GPtrArray * args,
    gpointer user_data);
static void send_publish (GTask * task);
static void publish_done (const gchar * command_name, GPtrArray * args,
    gpointer user_data);
static void new_connect (GstRtmp2Sink * rtmp2sink);
static void connect_task_done (GObject * object, GAsyncResult * result,
    gpointer user_data);


enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_HOST,
  PROP_PORT,
  PROP_APPLICATION,
  PROP_STREAM,
  PROP_SECURE_TOKEN,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_AUTHMOD,
  PROP_TIMEOUT,
};

#define DEFAULT_PUBLISHING_TYPE "live"

/* pad templates */

static GstStaticPadTemplate gst_rtmp2_sink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-flv")
    );

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstRtmp2Sink, gst_rtmp2_sink, GST_TYPE_BASE_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_rtmp2_sink_uri_handler_init);
    G_IMPLEMENT_INTERFACE (GST_TYPE_RTMP_LOCATION_HANDLER, NULL);
    )

     static void gst_rtmp2_sink_class_init (GstRtmp2SinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_rtmp2_sink_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "RTMP sink element", "Sink", "Sink element for RTMP streams",
      "David Schleef <ds@schleef.org>");

  gobject_class->set_property = gst_rtmp2_sink_set_property;
  gobject_class->get_property = gst_rtmp2_sink_get_property;
  gobject_class->finalize = gst_rtmp2_sink_finalize;
  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_rtmp2_sink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_rtmp2_sink_stop);
  base_sink_class->unlock = GST_DEBUG_FUNCPTR (gst_rtmp2_sink_unlock);
  base_sink_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_rtmp2_sink_unlock_stop);
  base_sink_class->render = GST_DEBUG_FUNCPTR (gst_rtmp2_sink_render);
  base_sink_class->set_caps = GST_DEBUG_FUNCPTR (gst_rtmp2_sink_set_caps);

  g_object_class_override_property (gobject_class, PROP_LOCATION, "location");
  g_object_class_override_property (gobject_class, PROP_HOST, "host");
  g_object_class_override_property (gobject_class, PROP_PORT, "port");
  g_object_class_override_property (gobject_class, PROP_APPLICATION,
      "application");
  g_object_class_override_property (gobject_class, PROP_STREAM, "stream");
  g_object_class_override_property (gobject_class, PROP_SECURE_TOKEN,
      "secure-token");
  g_object_class_override_property (gobject_class, PROP_USERNAME, "username");
  g_object_class_override_property (gobject_class, PROP_PASSWORD, "password");
  g_object_class_override_property (gobject_class, PROP_AUTHMOD, "authmod");
  g_object_class_override_property (gobject_class, PROP_TIMEOUT, "timeout");

  GST_DEBUG_CATEGORY_INIT (gst_rtmp2_sink_debug_category, "rtmp2sink", 0,
      "debug category for rtmp2sink element");
}

static void
gst_rtmp2_sink_init (GstRtmp2Sink * rtmp2sink)
{
  g_mutex_init (&rtmp2sink->lock);
  g_cond_init (&rtmp2sink->cond);
  rtmp2sink->task = gst_task_new (gst_rtmp2_sink_task, rtmp2sink, NULL);
  g_rec_mutex_init (&rtmp2sink->task_lock);
  gst_task_set_lock (rtmp2sink->task, &rtmp2sink->task_lock);
  rtmp2sink->headers =
      g_ptr_array_new_with_free_func ((GDestroyNotify) gst_mini_object_unref);
}

static void
gst_rtmp2_sink_uri_handler_init (GstURIHandlerInterface * iface)
{
  gst_rtmp_location_handler_implement_uri_handler (iface, GST_URI_SINK);
}

void
gst_rtmp2_sink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRtmp2Sink *self = GST_RTMP2_SINK (object);

  switch (property_id) {
    case PROP_LOCATION:
      gst_rtmp_location_handler_set_uri (GST_RTMP_LOCATION_HANDLER (self),
          g_value_get_string (value));
      break;
    case PROP_HOST:
      GST_OBJECT_LOCK (self);
      g_free (self->location.host);
      self->location.host = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_PORT:
      GST_OBJECT_LOCK (self);
      self->location.port = g_value_get_int (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_APPLICATION:
      GST_OBJECT_LOCK (self);
      g_free (self->location.application);
      self->location.application = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_STREAM:
      GST_OBJECT_LOCK (self);
      g_free (self->location.stream);
      self->location.stream = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_SECURE_TOKEN:
      GST_OBJECT_LOCK (self);
      g_free (self->location.secure_token);
      self->location.secure_token = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_USERNAME:
      GST_OBJECT_LOCK (self);
      g_free (self->location.username);
      self->location.username = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_PASSWORD:
      GST_OBJECT_LOCK (self);
      g_free (self->location.password);
      self->location.password = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_AUTHMOD:
    {
      GstRtmpAuthmod mode = g_value_get_enum (value);
      GST_OBJECT_LOCK (self);
      if (self->location.authmod != mode) {
        self->location.authmod = mode;
        GST_INFO_OBJECT (self, "successfully set auth method to (%i)", mode);
      }
      GST_OBJECT_UNLOCK (self);
      break;
    }
    case PROP_TIMEOUT:
      GST_OBJECT_LOCK (self);
      self->location.timeout = g_value_get_uint (value);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_rtmp2_sink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstRtmp2Sink *self = GST_RTMP2_SINK (object);

  switch (property_id) {
    case PROP_LOCATION:
      GST_OBJECT_LOCK (self);
      g_value_take_string (value, gst_rtmp_location_get_string (&self->location,
              TRUE));
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_HOST:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->location.host);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_PORT:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->location.port);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_APPLICATION:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->location.application);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_STREAM:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->location.stream);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_SECURE_TOKEN:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->location.secure_token);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_USERNAME:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->location.username);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_PASSWORD:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->location.password);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_AUTHMOD:
      GST_OBJECT_LOCK (self);
      g_value_set_enum (value, self->location.authmod);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_TIMEOUT:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->location.timeout);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_rtmp2_sink_finalize (GObject * object)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (object);

  /* clean up object here */
  g_clear_object (&rtmp2sink->connect_task);
  g_clear_object (&rtmp2sink->connection);
  g_clear_pointer (&rtmp2sink->headers, g_ptr_array_unref);
  gst_rtmp_location_clear (&rtmp2sink->location);
  g_clear_object (&rtmp2sink->task);
  g_rec_mutex_clear (&rtmp2sink->task_lock);
  g_mutex_clear (&rtmp2sink->lock);
  g_cond_clear (&rtmp2sink->cond);

  G_OBJECT_CLASS (gst_rtmp2_sink_parent_class)->finalize (object);
}

/* start and stop processing, ideal for opening/closing the resource */
static gboolean
gst_rtmp2_sink_start (GstBaseSink * sink)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (sink);

  GST_DEBUG_OBJECT (rtmp2sink, "start");

  rtmp2sink->running = TRUE;
  rtmp2sink->last_ts = 0;
  rtmp2sink->base_ts = 0;
  gst_task_start (rtmp2sink->task);

  return TRUE;
}

static gboolean
stop_main_loop (gpointer user_data)
{
  g_main_loop_quit (user_data);
  return G_SOURCE_REMOVE;
}

static gboolean
gst_rtmp2_sink_stop (GstBaseSink * sink)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (sink);

  GST_DEBUG_OBJECT (rtmp2sink, "stop");

  g_mutex_lock (&rtmp2sink->lock);

  gst_task_stop (rtmp2sink->task);
  rtmp2sink->running = FALSE;

  if (rtmp2sink->connect_task) {
    g_cancellable_cancel (g_task_get_cancellable (rtmp2sink->connect_task));
  }
  if (rtmp2sink->task_main_loop) {
    g_main_context_invoke (rtmp2sink->task_main_context, stop_main_loop,
        rtmp2sink->task_main_loop);
  }

  g_cond_broadcast (&rtmp2sink->cond);
  g_mutex_unlock (&rtmp2sink->lock);

  gst_task_join (rtmp2sink->task);

  return TRUE;
}

/* unlock any pending access to the resource. subclasses should unlock
 * any function ASAP. */
static gboolean
gst_rtmp2_sink_unlock (GstBaseSink * sink)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (sink);

  GST_DEBUG_OBJECT (rtmp2sink, "unlock");

  g_mutex_lock (&rtmp2sink->lock);
  rtmp2sink->flushing = TRUE;
  g_cond_broadcast (&rtmp2sink->cond);
  g_mutex_unlock (&rtmp2sink->lock);

  return TRUE;
}

static gboolean
gst_rtmp2_sink_unlock_stop (GstBaseSink * sink)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (sink);

  GST_DEBUG_OBJECT (rtmp2sink, "unlock_stop");

  g_mutex_lock (&rtmp2sink->lock);
  rtmp2sink->flushing = FALSE;
  g_mutex_unlock (&rtmp2sink->lock);

  return TRUE;
}

static gboolean
buffer_to_message (GstRtmp2Sink * self, GstBuffer * buffer, GstBuffer ** outbuf)
{
  GstBuffer *message;
  gsize payload_offset, payload_size;
  guint64 timestamp;
  guint32 cstream;
  GstRtmpMessageType type;

  {
    GstMapInfo info;

    if (G_UNLIKELY (!gst_buffer_map (buffer, &info, GST_MAP_READ))) {
      GST_ERROR_OBJECT (self, "map failed: %" GST_PTR_FORMAT, buffer);
      return FALSE;
    }

    /* FIXME: This is ugly and only works behind flvmux.
     *        Implement true RTMP muxing. */

    if (G_UNLIKELY (info.size >= 4 && memcmp (info.data, "FLV", 3) == 0)) {
      /* drop the header, we don't need it */
      GST_DEBUG_OBJECT (self, "ignoring FLV header: %" GST_PTR_FORMAT, buffer);
      gst_buffer_unmap (buffer, &info);
      *outbuf = NULL;
      return TRUE;
    }

    if (G_UNLIKELY (info.size < 11 + 4)) {
      GST_ERROR_OBJECT (self, "too small: %" GST_PTR_FORMAT, buffer);
      gst_buffer_unmap (buffer, &info);
      return FALSE;
    }

    /* payload between 11 byte header and 4 byte size footer */
    payload_offset = 11;
    payload_size = info.size - 11 - 4;

    type = GST_READ_UINT8 (info.data);
    timestamp = GST_READ_UINT24_BE (info.data + 4);
    timestamp |= (guint32) GST_READ_UINT8 (info.data + 7) << 24;

    /* flvmux timestamps roll over after about 49 days */
    if (timestamp + self->base_ts + G_MAXINT32 < self->last_ts) {
      GST_WARNING_OBJECT (self, "Timestamp regression %" G_GUINT64_FORMAT
          " -> %" G_GUINT64_FORMAT "; assuming overflow", self->last_ts,
          timestamp + self->base_ts);
      self->base_ts += G_MAXUINT32;
      self->base_ts += 1;
    } else if (timestamp + self->base_ts > self->last_ts + G_MAXINT32) {
      GST_WARNING_OBJECT (self, "Timestamp jump %" G_GUINT64_FORMAT
          " -> %" G_GUINT64_FORMAT "; assuming underflow", self->last_ts,
          timestamp + self->base_ts);
      if (self->base_ts > 0) {
        self->base_ts -= G_MAXUINT32;
        self->base_ts -= 1;
      } else {
        GST_WARNING_OBJECT (self, "Cannot regress further;"
            " forcing timestamp to zero");
        timestamp = 0;
      }
    }
    timestamp += self->base_ts;
    self->last_ts = timestamp;

    gst_buffer_unmap (buffer, &info);
  }

  switch (type) {
    case GST_RTMP_MESSAGE_TYPE_DATA_AMF0:
      cstream = 4;
      break;

    case GST_RTMP_MESSAGE_TYPE_AUDIO:
      cstream = 5;
      break;

    case GST_RTMP_MESSAGE_TYPE_VIDEO:
      cstream = 6;
      break;

    default:
      GST_ERROR_OBJECT (self, "unknown tag type %d", type);
      return FALSE;
  }

  message = gst_rtmp_message_new (type, cstream, 1);
  message = gst_buffer_append_region (message, gst_buffer_ref (buffer),
      payload_offset, payload_size);

  GST_BUFFER_DTS (message) = timestamp * GST_MSECOND;

  if (type == GST_RTMP_MESSAGE_TYPE_DATA_AMF0) {
    /* FIXME HACK, attach a setDataFrame header.  This should be done
     * using a command. */

    static const guint8 header[] = {
      0x02, 0x00, 0x0d, 0x40, 0x73, 0x65, 0x74, 0x44,
      0x61, 0x74, 0x61, 0x46, 0x72, 0x61, 0x6d, 0x65
    };

    GstMemory *memory = gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY,
        (guint8 *) header, sizeof header, 0, sizeof header, NULL, NULL);

    gst_buffer_prepend_memory (message, memory);
  }

  *outbuf = message;
  return TRUE;
}

static gboolean
should_drop_header (GstRtmp2Sink * self, GstBuffer * buffer)
{
  guint len;

  if (G_LIKELY (!GST_BUFFER_FLAG_IS_SET (buffer, GST_BUFFER_FLAG_HEADER))) {
    return FALSE;
  }

  g_mutex_lock (&self->lock);
  len = self->headers->len;
  g_mutex_unlock (&self->lock);

  /* Drop header buffers when we have streamheader caps */
  return len > 0;
}

static void
send_streamheader (GstRtmp2Sink * self)
{
  guint i;

  if (G_LIKELY (self->headers->len == 0)) {
    return;
  }

  GST_DEBUG_OBJECT (self, "Sending %u streamheader chunks", self->headers->len);

  for (i = 0; i < self->headers->len; i++) {
    gst_rtmp_connection_queue_message (self->connection,
        g_ptr_array_index (self->headers, i));
  }

  /* Steal pointers: suppress free */
  g_ptr_array_set_free_func (self->headers, NULL);
  g_ptr_array_set_size (self->headers, 0);
  g_ptr_array_set_free_func (self->headers,
      (GDestroyNotify) gst_mini_object_unref);
}

static GstFlowReturn
send_message (GstRtmp2Sink * self, GstBuffer * message)
{
  GstFlowReturn ret;

  g_mutex_lock (&self->lock);

  while (G_UNLIKELY (!self->flushing && !self->connection &&
          self->connect_task)) {
    GST_DEBUG_OBJECT (self, "waiting for connection");
    g_cond_wait (&self->cond, &self->lock);
  }

  while (G_UNLIKELY (!self->flushing && self->connection &&
          gst_rtmp_connection_get_num_queued (self->connection) > 3)) {
    GST_LOG_OBJECT (self, "waiting for queue");
    g_cond_wait (&self->cond, &self->lock);
  }

  if (G_UNLIKELY (self->flushing)) {
    gst_buffer_unref (message);
    ret = GST_FLOW_FLUSHING;
  } else if (G_UNLIKELY (!self->connection)) {
    gst_buffer_unref (message);
    ret = GST_FLOW_ERROR;
  } else {
    send_streamheader (self);
    gst_rtmp_connection_queue_message (self->connection, message);
    ret = GST_FLOW_OK;
  }

  g_mutex_unlock (&self->lock);
  return ret;
}

static GstFlowReturn
gst_rtmp2_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstRtmp2Sink *self = GST_RTMP2_SINK (sink);
  GstBuffer *message;

  if (G_UNLIKELY (should_drop_header (self, buffer))) {
    GST_DEBUG_OBJECT (self, "Skipping header %" GST_PTR_FORMAT, buffer);
    return GST_FLOW_OK;
  }

  GST_LOG_OBJECT (self, "render %" GST_PTR_FORMAT, buffer);

  if (G_UNLIKELY (!buffer_to_message (self, buffer, &message))) {
    GST_ERROR_OBJECT (self, "Failed to read %" GST_PTR_FORMAT, buffer);
    return GST_FLOW_ERROR;
  }

  if (G_UNLIKELY (!message)) {
    GST_DEBUG_OBJECT (self, "Skipping %" GST_PTR_FORMAT, buffer);
    return GST_FLOW_OK;
  }

  return send_message (self, message);
}

static const GArray *
caps_get_streamheader (GstCaps * caps)
{
  GstStructure *s = gst_caps_get_structure (caps, 0);
  const GValue *v = gst_structure_get_value (s, "streamheader");
  return v ? g_value_peek_pointer (v) : NULL;
}

static gboolean
gst_rtmp2_sink_set_caps (GstBaseSink * sink, GstCaps * caps)
{
  GstRtmp2Sink *self = GST_RTMP2_SINK (sink);
  const GArray *streamheader;
  guint i;

  GST_DEBUG_OBJECT (self, "setcaps %" GST_PTR_FORMAT, caps);

  g_ptr_array_set_size (self->headers, 0);

  streamheader = caps_get_streamheader (caps);
  for (i = 0; i < streamheader->len; i++) {
    GstBuffer *buffer =
        g_value_peek_pointer (&g_array_index (streamheader, GValue, i));
    GstBuffer *message;

    if (!buffer_to_message (self, buffer, &message)) {
      GST_ERROR_OBJECT (self, "Failed to read streamheader %" GST_PTR_FORMAT,
          buffer);
      return FALSE;
    }

    if (!message) {
      GST_DEBUG_OBJECT (self, "Skipping streamheader %" GST_PTR_FORMAT, buffer);
      continue;
    }

    GST_DEBUG_OBJECT (self, "Adding streamheader %" GST_PTR_FORMAT, buffer);
    g_ptr_array_add (self->headers, message);
  }

  GST_DEBUG_OBJECT (self, "Collected streamheaders: %u buffers -> %u chunks",
      i, self->headers->len);

  return TRUE;
}

/* Mainloop task */
static void
gst_rtmp2_sink_task (gpointer user_data)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (user_data);
  GMainContext *main_context;
  GMainLoop *main_loop;

  GST_DEBUG ("gst_rtmp2_sink_task starting");

  g_mutex_lock (&rtmp2sink->lock);
  main_context = rtmp2sink->task_main_context = g_main_context_new ();
  g_main_context_push_thread_default (main_context);
  main_loop = rtmp2sink->task_main_loop = g_main_loop_new (main_context, TRUE);
  new_connect (rtmp2sink);
  g_mutex_unlock (&rtmp2sink->lock);

  g_main_loop_run (main_loop);

  g_mutex_lock (&rtmp2sink->lock);
  g_clear_pointer (&rtmp2sink->task_main_loop, g_main_loop_unref);
  g_clear_pointer (&rtmp2sink->connection, gst_rtmp_connection_close_and_unref);
  g_cond_broadcast (&rtmp2sink->cond);
  g_mutex_unlock (&rtmp2sink->lock);

  while (g_main_context_pending (main_context)) {
    GST_DEBUG ("iterating main context to clean up");
    g_main_context_iteration (main_context, FALSE);
  }

  g_main_context_pop_thread_default (main_context);

  g_mutex_lock (&rtmp2sink->lock);
  g_clear_pointer (&rtmp2sink->task_main_context, g_main_context_unref);
  g_ptr_array_set_size (rtmp2sink->headers, 0);
  g_mutex_unlock (&rtmp2sink->lock);

  GST_DEBUG ("gst_rtmp2_sink_task exiting");
}

static void
new_connect (GstRtmp2Sink * rtmp2sink)
{
  GCancellable *cancellable = g_cancellable_new ();

  g_warn_if_fail (!rtmp2sink->connect_task);
  rtmp2sink->connect_task =
      g_task_new (rtmp2sink, cancellable, connect_task_done, NULL);

  GST_OBJECT_LOCK (rtmp2sink);
  gst_rtmp_client_connect_async (&rtmp2sink->location,
      cancellable, client_connect_done, rtmp2sink->connect_task);
  GST_OBJECT_UNLOCK (rtmp2sink);

  g_object_unref (cancellable);
}

static void
put_chunk (GstRtmpConnection * connection, gpointer user_data)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (user_data);

  g_mutex_lock (&rtmp2sink->lock);
  g_cond_signal (&rtmp2sink->cond);
  g_mutex_unlock (&rtmp2sink->lock);
}

static void
connection_error (GstRtmpConnection * connection, GstRtmp2Sink * rtmp2sink)
{
  g_mutex_lock (&rtmp2sink->lock);
  if (rtmp2sink->connect_task) {
    g_cancellable_cancel (g_task_get_cancellable (rtmp2sink->connect_task));
  } else if (rtmp2sink->task_main_loop) {
    GST_ELEMENT_ERROR (rtmp2sink, RESOURCE, WRITE,
        ("Connection error"), (NULL));
    gst_task_stop (rtmp2sink->task);
    g_main_loop_quit (rtmp2sink->task_main_loop);
  }
  g_mutex_unlock (&rtmp2sink->lock);
}

static void
connect_task_done (GObject * object, GAsyncResult * result, gpointer user_data)
{
  GstRtmp2Sink *rtmp2sink = GST_RTMP2_SINK (object);
  GTask *task = G_TASK (result);
  GError *error = NULL;

  g_mutex_lock (&rtmp2sink->lock);

  g_warn_if_fail (rtmp2sink->connect_task == task);
  g_warn_if_fail (g_task_is_valid (task, object));

  rtmp2sink->connect_task = NULL;
  rtmp2sink->connection = g_task_propagate_pointer (task, &error);
  if (rtmp2sink->connection) {
    gst_rtmp_connection_set_output_handler (rtmp2sink->connection,
        put_chunk, g_object_ref (rtmp2sink), g_object_unref);
    g_signal_connect_object (rtmp2sink->connection, "error",
        G_CALLBACK (connection_error), rtmp2sink, 0);
  } else {
    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
      GST_ELEMENT_ERROR (rtmp2sink, RESOURCE, NOT_AUTHORIZED,
          ("Not authorized to push to server"),
          ("%s", error ? GST_STR_NULL (error->message) : "(NULL error)"));
    } else if (g_error_matches (error, G_IO_ERROR,
            G_IO_ERROR_CONNECTION_REFUSED)) {
      GST_ELEMENT_ERROR (rtmp2sink, RESOURCE, OPEN_READ,
          ("Could not connect to server"), ("%s",
              error ? GST_STR_NULL (error->message) : "(NULL error)"));
    } else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GST_ELEMENT_ERROR (rtmp2sink, RESOURCE, FAILED,
          ("Could not connect to server"),
          ("%s", error ? GST_STR_NULL (error->message) : "(NULL error)"));
    }
  }

  g_cond_signal (&rtmp2sink->cond);
  g_mutex_unlock (&rtmp2sink->lock);
}

static void
client_connect_done (GObject * source, GAsyncResult * result,
    gpointer user_data)
{
  GTask *task = user_data;
  GError *error = NULL;
  GstRtmpConnection *connection;

  connection = gst_rtmp_client_connect_finish (result, &error);
  if (!connection) {
    g_task_return_error (task, error);
    g_object_unref (task);
    return;
  }

  g_task_set_task_data (task, connection, g_object_unref);

  if (g_task_return_error_if_cancelled (task)) {
    g_object_unref (task);
    return;
  }

  send_create_stream (task);
}

static void
send_create_stream (GTask * task)
{
  GstRtmpConnection *connection = g_task_get_task_data (task);
  GstRtmp2Sink *rtmp2sink = g_task_get_source_object (task);
  GstAmfNode *node;
  GstAmfNode *node2;

  node = gst_amf_node_new_null ();

  GST_OBJECT_LOCK (rtmp2sink);
  node2 = gst_amf_node_new_string (rtmp2sink->location.stream);
  GST_OBJECT_UNLOCK (rtmp2sink);

  gst_rtmp_connection_send_command (connection, NULL, NULL, 0,
      "releaseStream", node, node2, NULL);
  gst_rtmp_connection_send_command (connection, NULL, NULL, 0,
      "FCPublish", node, node2, NULL);
  gst_rtmp_connection_send_command (connection, create_stream_done, task, 0,
      "createStream", node, NULL);

  gst_amf_node_free (node2);
  gst_amf_node_free (node);
}

static void
create_stream_done (const gchar * command_name, GPtrArray * args,
    gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  GstRtmp2Sink *rtmp2sink = g_task_get_source_object (task);

  if (g_task_return_error_if_cancelled (task)) {
    g_object_unref (task);
    return;
  }

  if (!args || args->len < 2) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
        "createStream failed");
    g_object_unref (task);
    return;
  }

  GST_DEBUG_OBJECT (rtmp2sink, "createStream success, stream_id=%.0f",
      gst_amf_node_get_number (g_ptr_array_index (args, 1)));

  if (g_task_return_error_if_cancelled (task)) {
    g_object_unref (task);
    return;
  }

  send_publish (task);
}

static void
send_publish (GTask * task)
{
  GstRtmpConnection *connection = g_task_get_task_data (task);
  GstRtmp2Sink *rtmp2sink = g_task_get_source_object (task);
  GstAmfNode *node;
  GstAmfNode *node2;
  GstAmfNode *node3;

  gst_rtmp_connection_expect_command (connection, publish_done, task, 1,
      "onStatus");

  node = gst_amf_node_new_null ();
  node2 = gst_amf_node_new_string (rtmp2sink->location.stream);
  node3 = gst_amf_node_new_string (DEFAULT_PUBLISHING_TYPE);
  gst_rtmp_connection_send_command (connection, NULL, NULL, 1,
      "publish", node, node2, node3, NULL);
  gst_amf_node_free (node);
  gst_amf_node_free (node2);
  gst_amf_node_free (node3);
}

static void
publish_done (const gchar * command_name, GPtrArray * args, gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  GstRtmpConnection *connection = g_task_get_task_data (task);
  GstRtmp2Sink *rtmp2sink = g_task_get_source_object (task);
  GstAmfType optional_args_type = GST_AMF_TYPE_INVALID;
  GstAmfNode *optional_args;

  if (g_task_return_error_if_cancelled (task)) {
    g_object_unref (task);
    return;
  }

  if (!args || args->len < 1) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
        "publish failed");
    g_object_unref (task);
    return;
  }

  if (args->len > 1) {
    optional_args = g_ptr_array_index (args, 1);
    optional_args_type = gst_amf_node_get_type (optional_args);
  }

  switch (optional_args_type) {
    case GST_AMF_TYPE_OBJECT:{
      const GstAmfNode *node = gst_amf_node_get_field (optional_args, "code");
      const gchar *code = node ? gst_amf_node_peek_string (node) : NULL;

      if (g_str_equal (code, "NetStream.Publish.Start")) {
        GST_DEBUG_OBJECT (rtmp2sink, "publish success, code=%s", code);
        g_task_return_pointer (task, g_object_ref (connection),
            gst_rtmp_connection_close_and_unref);
      } else if (g_str_equal (code, "NetStream.Publish.BadName")) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_EXISTS,
            "Stream already exists! (%s)", code);
      } else if (g_str_equal (code, "NetStream.Publish.Denied")) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
            "Publish denied! (%s)", code);
      } else {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "unhandled publish result code: %s", GST_STR_NULL (code));
      }
      break;
    }

    default:
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
          "publish failed");
      break;
  }

  g_task_set_task_data (task, NULL, NULL);
  g_object_unref (task);
}
