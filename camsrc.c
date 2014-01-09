/*
 * Things to do:
 * * protect against multiple overlapping requests
 * * on startup, choose a camera
 * * adjust start time to account for keyframes
 * * record last PTS seen and use that as a lower bound for acceptable request start-time
 * */

#include <gst/gst.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define PORT 2000
#define DEVICE_NUMBER_TEST -1
#define GST_QUEUE_LEAK_DOWNSTREAM 2

typedef struct {
  GMainLoop  *loop;
  GstElement *pipeline;
  GstElement *queue2;
  GstElement *bin;
  GstPad * blockpad;
  GstPad * srcpad;
  gulong blockpad_probe_id;
  gulong srcpad_probe_id;
  GstClockTime base_time;
  GstClockTime clock_start;
  GstClockTime clock_end;
  GstClockTime clock_desired_duration;
  GSocketConnection * connection;
  guint socket_watcher_id;
  gchar file_location[1024];
} App;

typedef enum {
  WINDOW_BEFORE,
  WINDOW_INSIDE_KEYFRAME,
  WINDOW_INSIDE,
  WINDOW_AFTER
} WindowReturn;


// Set up debug output
GST_DEBUG_CATEGORY_STATIC (camsrc);
#define GST_CAT_DEFAULT camsrc

static gchar * get_buffer_status(gchar * buff, App * app)
{
  GstClockTime level_time_1;
  GstClockTime level_time_2;
  guint level_bytes_1, level_buffers_1;
  guint level_bytes_2, level_buffers_2;

  g_object_get (gst_bin_get_by_name (GST_BIN (app->pipeline), "upstream-queue"),
      "current-level-time", &level_time_1,
      "current-level-bytes", &level_bytes_1,
      "current-level-buffers", &level_buffers_1, NULL);

  g_object_get (app->queue2,
      "current-level-time", &level_time_2,
      "current-level-bytes", &level_bytes_2,
      "current-level-buffers", &level_buffers_2, NULL);

  g_snprintf(buff, 1024,
      "queue1 reports %lums, %u buffers, %u bytes "
      "queue2 reports %lums, %u buffers, %u bytes\n",
      GST_TIME_AS_MSECONDS(level_time_1), level_buffers_1, level_bytes_1,
      GST_TIME_AS_MSECONDS(level_time_2), level_buffers_2, level_bytes_2);

  return buff;
}


gboolean mkpath(gchar* file_path, mode_t mode) {
  if (!file_path || !*file_path)
    return FALSE;

  gchar* p;
  for (p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
    *p= '\0';
    if (mkdir(file_path, mode) == -1) {
      if (errno != EEXIST) { perror(file_path); *p='/'; return FALSE; }
    }
    *p = '/';
  }
  return TRUE;
}

static GstElement * create_bin (App * app)
{
  GST_DEBUG ("Saving stream to %s...", app->file_location);

  GstElement *bin = gst_element_factory_make ("bin", NULL),
             *mux = gst_element_factory_make ("mp4mux", "mux"),
             *sink = gst_element_factory_make ("filesink", "sink");

  if (!bin) { GST_ERROR("Failed to create bin"); }
  if (!mux) { GST_ERROR("Failed to create mux"); }
  if (!sink) { GST_ERROR("Failed to create sink"); }

  if (!bin || !mux || !sink) {
    return NULL;
  }

  if (!mkpath(app->file_location, 0766)) {
    GST_ERROR ("mkpath of '%s' failed", app->file_location);
  }

  g_object_set (sink, "location", app->file_location, NULL);

  gst_bin_add_many (GST_BIN (bin), mux, sink, NULL);
  gst_element_link (mux, sink);
  gst_element_add_pad(bin, gst_ghost_pad_new ("sink", gst_element_get_request_pad (mux, "video_%u")));

  return bin;
}

static void drop_bin (App * app)
{
  gst_element_set_state (app->bin, GST_STATE_NULL);
  gst_bin_remove (GST_BIN(app->pipeline), app->bin);
}

static void hangup (App * app)
{
  if (app->connection) {
    g_source_remove (app->socket_watcher_id);
    g_object_unref (app->connection);
    app->connection = NULL;
  }
}

static gint get_file_descriptor (App * app)
{
  gint fd = app->connection ?
    g_socket_get_fd (g_socket_connection_get_socket (app->connection)) :
    g_open ("/dev/null", O_WRONLY, 0);
  return fd;
}

static gboolean socket_send_string (gchar * str, App * app)
{
  if (!app->connection || !str)
    return FALSE;

  ssize_t sent;
  guint len = strlen(str);
  gint dest = get_file_descriptor (app);

  if ((sent = write (dest, str, len)) < len) {
    GST_ERROR ("entire response didn't get sent (only %zd bytes of %u).", sent, len);
    return FALSE;
  }
  return TRUE;
}

static void send_result_to_socket (App * app)
{
  gint src;
  struct stat stat_buf;
  gchar response[1024];

  src = g_open (app->file_location, O_RDONLY);
  fstat (src, &stat_buf);
  close (src);

  g_snprintf (response, sizeof(response),
      "{ \"status\": 200, \"content-type\": \"video/mp4\", \"content-length\": %lld, \"location\": \"%s\" }\n",
      stat_buf.st_size, app->file_location);

  socket_send_string (response, app);
}

static void send_error_to_socket (guint status, gchar * reason, App *app)
{
  gchar response[1024];

  g_snprintf (response, sizeof(response),
      "{ \"status\": %u, \"reason\": \"%s\" }", status, reason);

  socket_send_string (response, app);
}

static GstPadProbeReturn
blockpad_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  App * app = user_data;
  GstElement * mux = gst_bin_get_by_name (GST_BIN(app->bin), "mux");

  GstPad * mux_sink_pad = gst_element_get_static_pad (mux, "video_0");
    gst_pad_send_event (mux_sink_pad, gst_event_new_eos());
  g_object_unref (mux_sink_pad);

  return GST_PAD_PROBE_OK;
}

static void block_pipeline(App *app)
{
  app->blockpad_probe_id = gst_pad_add_probe (app->blockpad,
      GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER,
      blockpad_probe_cb, app, NULL);
}

static WindowReturn inside_window(GstPadProbeInfo * info, App * app)
{
  GstClockTime pts = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info));
  GstBufferFlags flags = GST_BUFFER_FLAGS(GST_PAD_PROBE_INFO_BUFFER (info));
  gboolean have_keyframe = (flags & GST_BUFFER_FLAG_DELTA_UNIT) != GST_BUFFER_FLAG_DELTA_UNIT;

  WindowReturn ret = pts < app->clock_start ?
    WINDOW_BEFORE : pts >= app->clock_end ?
    WINDOW_AFTER : have_keyframe ?
    WINDOW_INSIDE_KEYFRAME : WINDOW_INSIDE;

  if (have_keyframe)
    GST_LOG ("pts check : %lu%s %s [%lu, %lu]", GST_TIME_AS_MSECONDS(pts), have_keyframe ? "*" : "",
        ret == WINDOW_BEFORE ? "before" : ret == WINDOW_AFTER ? "after" : "inside",
        GST_TIME_AS_MSECONDS(app->clock_start), GST_TIME_AS_MSECONDS(app->clock_end));

  return ret;
}

static GstPadProbeReturn
wait_for_end_cb (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  App * app = data;


  switch (inside_window(info, app)) {
    case WINDOW_BEFORE:
      GST_ERROR ("Somehow we're before our window looking for end");
      break;

    case WINDOW_INSIDE:
    case WINDOW_INSIDE_KEYFRAME:
      GST_LOG ("Passing along a frame that is in window");
      return GST_PAD_PROBE_PASS;

    case WINDOW_AFTER:
      break;
  }

  gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));

  block_pipeline(app);

  return GST_PAD_PROBE_PASS;
}


static GstPadProbeReturn
wait_for_start_cb (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  App * app = data;

  switch (inside_window(info, app)) {
    case WINDOW_BEFORE:
      GST_LOG ("Dropping a frame that is too old");
      return GST_PAD_PROBE_DROP;

    case WINDOW_INSIDE:
      GST_LOG ("Dropping non-keyframe");
      return GST_PAD_PROBE_DROP;

    case WINDOW_INSIDE_KEYFRAME:
      GST_DEBUG ("Found a key frame that is in range");
      app->clock_end = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info)) + app->clock_desired_duration;
      gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));
      gst_pad_add_probe (app->blockpad, GST_PAD_PROBE_TYPE_BUFFER, wait_for_end_cb, app, NULL);
      return GST_PAD_PROBE_PASS;

    case WINDOW_AFTER:
      GST_WARNING ("Didn't find a keyframe in range!");
      gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));
      block_pipeline(app);
      return GST_PAD_PROBE_DROP;
  }
}


static GstPadProbeReturn
drop_query_cb (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  GstQuery * query = gst_pad_probe_info_get_query (info);

  GST_DEBUG ("Received query of type '%s'", GST_QUERY_TYPE_NAME (query));

  if (GST_QUERY_TYPE (query) == GST_QUERY_ALLOCATION) {
    GST_DEBUG ("Dropping query");
    return GST_PAD_PROBE_DROP;
  } else {
    GST_DEBUG ("Passing query");
    return GST_PAD_PROBE_OK;
  }

}

static void unblock_pipeline(App *app)
{
  // Need to prevent the source from sending an allocation query
  // because it will hang the upstream pipeline.
  // This gets added once (must happen after the initial caps
  // negotiation) and left in place forever.
  if (!app->srcpad_probe_id) {
    GST_DEBUG ("Adding srcpad_probe");
    app->srcpad_probe_id =  gst_pad_add_probe (app->srcpad,
        GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, drop_query_cb, app, NULL);
  }

  app->bin = create_bin (app);
  gst_bin_add (GST_BIN(app->pipeline), app->bin);
  gst_element_link (app->queue2, app->bin);
  gst_element_set_state (app->bin, GST_STATE_PLAYING);

  gst_pad_add_probe (app->blockpad,
      GST_PAD_PROBE_TYPE_BUFFER,
      wait_for_start_cb, app, NULL);

  if (app->blockpad_probe_id) {
    GST_DEBUG ("Unblocking pipeline");
    gst_pad_remove_probe (app->blockpad, app->blockpad_probe_id);
    app->blockpad_probe_id = 0;
  }
}

static GstClockTime get_current_time()
{
  GTimeVal current_time;

  g_get_current_time (&current_time);
  return GST_TIMEVAL_TO_TIME (current_time);
}

gboolean io_callback(GIOChannel *source, GIOCondition condition, gpointer data)
{
  GError *error = NULL;
  GString *buffer = g_string_new(NULL);
  App * app = data;

  switch (g_io_channel_read_line_string(source, buffer, NULL, &error)) {
    case G_IO_STATUS_NORMAL:
      g_strstrip(buffer->str);
      if (!strcmp (buffer->str, ""))
        break;
      GST_DEBUG("received command %s", buffer->str);
      if (!strcmp("shutdown", buffer->str)) {
        g_main_loop_quit (app->loop);
        hangup (app);
        return FALSE;

      } else if (!strcmp("query", buffer->str)) {
        gsize bytes_written;
        char buff[1024];

        g_io_channel_write_chars (source, get_buffer_status (buff, app), -1, &bytes_written, &error);
        g_io_channel_flush (source, &error);
        hangup (app);
        return FALSE;

      } else if (g_str_has_prefix(buffer->str, "replay ")) {
        glong start = 0;
        glong duration = 0;
        gchar * next = &buffer->str[7];
        gchar * filepath;
        gboolean valid = TRUE;

        // Parse out params: start duration filepath
        start = strtol(next, &next, 10);
        if (!start && errno == EINVAL)
          valid = FALSE;
        duration = strtol(next, &next, 10);
        if (duration <= 0)
          valid = FALSE;
        filepath = g_strstrip(next);
        if (filepath[0] != '/')
          valid = FALSE;

        if (!valid) {
          GST_WARNING ("command parameters invalid");
          send_error_to_socket (400, "couldn't parse request", app);
          hangup (app);
          return FALSE;
        }

        // Convert incoming times from msec to nsec
        start = start * GST_MSECOND;
        duration = duration * GST_MSECOND;

        // Start may be provided relative to current clock; convert to absolute
        if (start < 0)
          start += get_current_time();

        // Adjust start by base_time to get time relative to pipeline clock
        start -= app->base_time;

        GST_INFO ("%20lu: get_current_time()",  GST_TIME_AS_MSECONDS(get_current_time()));
        GST_INFO ("%20lu: base_time", GST_TIME_AS_MSECONDS(app->base_time));
        GST_INFO ("%20lu: get_current_time - base_time", GST_TIME_AS_MSECONDS(get_current_time() - app->base_time));
        GST_INFO ("%20ld: start", GST_TIME_AS_MSECONDS(start));

        app->clock_start = start;
        app->clock_desired_duration = duration;
        app->clock_end = start + duration;
        strcpy(app->file_location, filepath);

        // May not request clips from the future
        if (app->clock_start > get_current_time() || app->clock_end > get_current_time()) {
          GST_WARNING ("command parameters invalid");
          send_error_to_socket (416, "invalid time range requested", app);
          hangup (app);
          return FALSE;
        }

        unblock_pipeline(app);
      } else {
        GST_INFO ("Unrecognized command");
      }
      break;
    case G_IO_STATUS_ERROR:
      GST_ERROR ("G_IO_STATUS_ERROR: %s", error->message);
      g_error_free (error);
      hangup (app);
      return FALSE;
    case G_IO_STATUS_EOF:
      GST_INFO ("Client disappeared");
      hangup (app);
      return FALSE;
    case G_IO_STATUS_AGAIN:
      break;
    default:
      g_return_val_if_reached(FALSE);
  }
  return TRUE;
}

static gboolean
incoming_callback  (GSocketService *service,
                    GSocketConnection *connection,
                    GObject *source_object,
                    gpointer user_data)
{
  GError * error = NULL;
  App * app = user_data;

  GST_DEBUG ("Received connection from client");
  g_object_ref (connection);
  app->connection = connection;

  GIOChannel *channel = g_io_channel_unix_new (get_file_descriptor (app));
  app->socket_watcher_id = g_io_add_watch (channel, G_IO_IN, (GIOFunc) io_callback, app);
  g_io_channel_set_encoding (channel, NULL, &error);
  g_io_channel_set_close_on_unref (channel, TRUE);
  return TRUE;
}

static gboolean
bus_call (GstBus *bus, GstMessage *msg, gpointer data)
{
  App * app = data;
  gchar buffer[1024];

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      GST_DEBUG ("Finished writing stream to %s", app->file_location);
      GST_DEBUG ("buffer status is now: %s", get_buffer_status (buffer, app));
      send_result_to_socket (app);
      drop_bin (app);
      hangup (app);
      break;

    case GST_MESSAGE_ERROR:
      {
        gchar  *debug;
        GError *error;

        gst_message_parse_error (msg, &error, &debug);

        GST_ERROR ("Error: %s", error->message);
        g_error_free (error);

        GST_ERROR ("Debugging info: %s", (debug) ? debug : "none");
        g_free (debug);

        g_main_loop_quit (app->loop);
        break;
      }
    default:
      break;
  }

  return TRUE;
}

int
main (int argc, char *argv[])
{
  GstBus *bus;
  guint bus_watch_id;
  App app_data;
  App * app = &app_data;
  GOptionContext * option_context;
  GError * error = NULL;
  gint port = -1, device_number = DEVICE_NUMBER_TEST;

  GOptionEntry option_entries[] = {
    { "port", 'p', 0, G_OPTION_ARG_INT, &port, "Port to listen on (default 2000)", "PORT" },
    { "device-number", 'd', 0, G_OPTION_ARG_INT, &device_number, "Camera to use", "DEVICE_NUMBER" },
    { NULL }
  };

  gst_init (&argc, &argv);
  GST_DEBUG_CATEGORY_INIT (camsrc, "camsrc", 0, "camera source");

  option_context = g_option_context_new ("- start queue-buffered video server");
  g_option_context_add_main_entries (option_context, option_entries, NULL);
  g_option_context_parse (option_context, &argc, &argv, &error);
  g_option_context_free (option_context);

  // If no port specified, we use PORT + device_number (unless we're also testing,
  // in which case we just use PORT).
  if (port < 0) {
    port = device_number == DEVICE_NUMBER_TEST ? PORT : PORT + device_number;
  }

  /* set up socket */
  GSocketService * service = g_socket_service_new ();
  g_socket_listener_add_inet_port ((GSocketListener*)service, port, NULL, &error);

  if (error) {
    g_error (error->message);
  }

  g_signal_connect (service, "incoming", G_CALLBACK (incoming_callback), app);
  g_socket_service_start (service);

  app->loop = g_main_loop_new (NULL, FALSE);
  app->blockpad_probe_id = 0;
  app->srcpad_probe_id = 0;
  app->connection = NULL;
  strcpy(app->file_location, "/dev/null");

  /* Create gstreamer elements */
  app->pipeline          = gst_pipeline_new ("camsrc");

  GstElement * source,
             * filter    = gst_element_factory_make ("capsfilter", "caps-filter"),
             * videorate = gst_element_factory_make ("videorate", "video-rate"),
             * converter = gst_element_factory_make ("videoconvert", "video-convert"),
             * queue1    = gst_element_factory_make ("queue", "upstream-queue"),
             * encoder   = gst_element_factory_make ("x264enc", "video-encoder");

  app->queue2            = gst_element_factory_make ("queue", "ringbuffer-queue");
  app->bin               = create_bin (app);

  if (device_number == DEVICE_NUMBER_TEST) {
    source = gst_element_factory_make ("videotestsrc", "video-source");
    g_object_set (source, "is-live", TRUE, NULL);
    /*g_object_set (source, "pattern", 18, NULL);*/
  } else {
    source = gst_element_factory_make ("decklinksrc", "video-source");
    g_object_set (source,
        "device-number", device_number,
        "connection", 0,
        "mode", "1080p30",
        NULL);
  }

  if (!app->pipeline) { GST_ERROR ("Failed to create pipeline"); }
  if (!source) { GST_ERROR("failed to create videotestsrc"); }
  if (!filter) { GST_ERROR ("Failed to create capsfilter"); }
  if (!videorate) { GST_ERROR("failed to create video-rate"); }
  if (!converter) { GST_ERROR("failed to create videoconvert"); }
  if (!queue1) { GST_ERROR("failed to create upstream-queue"); }
  if (!encoder) { GST_ERROR("failed to create encoder"); }
  if (!app->queue2) { GST_ERROR("failed to create ringbuffer-queue"); }

  if (!app->pipeline || !source || !filter || !videorate || !converter ||
      !queue1 || !encoder || !app->queue2 || !app->bin) {
    GST_ERROR ("An element could not be created. Exiting.");
    return -1;
  }

  g_object_set (encoder, "byte-stream", TRUE, NULL);
  g_object_set (encoder, "key-int-max", 30, NULL);
  g_object_set (app->queue2,
      "leaky", GST_QUEUE_LEAK_DOWNSTREAM,
      "max-size-bytes", 0,
      "max-size-buffers", 0,
      "max-size-time", 5 * 60 * GST_SECOND,
      NULL);

  GstCaps * caps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "I420",
      "framerate", GST_TYPE_FRACTION, 30, 1,
      "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
      "width", G_TYPE_INT, 1920,
      "height", G_TYPE_INT, 1080,
      NULL);

  g_object_set (filter, "caps", caps, NULL);

  /* Add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, app);
  gst_object_unref (bus);

  gst_bin_add_many (GST_BIN (app->pipeline),
      source, filter, /* videorate, */ converter, queue1, encoder, app->queue2, app->bin, NULL);

  gst_element_link_many (source, filter, /* videorate, */ converter, queue1, encoder, app->queue2, app->bin, NULL);

  app->blockpad = gst_element_get_static_pad (app->queue2, "src");
  app->srcpad   = gst_element_get_static_pad (encoder, "src");

  block_pipeline(app);

   /*Verbose*/
  /*g_signal_connect (app->pipeline, "deep-notify",*/
    /*G_CALLBACK (gst_object_default_deep_notify), NULL);*/

  /* Set the pipeline to "playing" state */
  gst_element_set_state (app->pipeline, GST_STATE_PLAYING);

  app->base_time = get_current_time();

  g_printf ("camsrc listening on port %d...\n", port);

  g_main_loop_run (app->loop);

  /* Out of the main loop, clean up nicely */
  gst_element_set_state (app->pipeline, GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (app->pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (app->loop);

  return 0;
}
