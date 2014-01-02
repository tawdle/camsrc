/*
 * Things to do:
 * * protect against multiple overlapping requests
 * * on startup, choose a camera
 * X on startup, choose a named pipe
 * * adjust start time to account for keyframes
 * * in replay replay request, take some form of absolute datetime
 * * in replay request, take name of pipe for output
 * X look into ORC for Mac OS and Linux
 * */

#include <gst/gst.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

typedef enum {
  STATUS_IDLE,
  STATUS_WAITING_FOR_KEY_FRAME,
  STATUS_STREAMING,
  STATUS_WAITING_FOR_EOS
} app_status;

typedef struct {
  GMainLoop  *loop;
  GstElement *pipeline;
  GstElement *source;
  GstElement *caps;
  GstElement *videorate;
  GstElement *converter;
  GstElement *queue1;
  GstElement *encoder;
  GstElement *queue2;
  GstElement *bin;
  GstElement *mux;
  app_status status;
  GstPad * blockpad;
  guint file_count;
  gboolean is_blocking;
  gulong blockpad_probe_id;
  GstClockTime base_time;
  GstClockTime clock_start;
  GstClockTime clock_end;
  GstClockTime clock_desired_duration;
} app_data;

app_data global_app_data;

void set_state_and_wait (GstElement * element, GstState state, app_data * app)
{
  GstState new_state;

  g_printf ("set_state_and_wait: setting state to %s\n", gst_element_state_get_name (state));

  switch (gst_element_set_state (element, state)) {
    case GST_STATE_CHANGE_SUCCESS:
      g_printf ("Got immediate state change success\n");
      break;
    case GST_STATE_CHANGE_ASYNC:
      g_print ("Waiting for async state change... ");
      if (gst_element_get_state (element, &new_state, NULL, 1 * GST_SECOND) !=
          GST_STATE_CHANGE_SUCCESS || new_state != state) {
        g_printerr ("sink failed to change state to %s; is stuck at %s\n", gst_element_state_get_name (state), gst_element_state_get_name (new_state));
        /*g_main_loop_quit (app->loop);*/
        return;
      }
      g_print ("got it.\n");
      break;
    case GST_STATE_CHANGE_FAILURE:
      g_printerr ("state change failure");
      g_main_loop_quit (app->loop);
      return;
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
      g_print ("no prerolll\n");
      break;
  }
}

void drop_bin (app_data * app)
{
  gst_element_set_state (app->bin, GST_STATE_NULL);
  gst_bin_remove (GST_BIN(app->pipeline), app->bin);
}


static gboolean
bus_call (GstBus     *bus,
    GstMessage *msg,
    gpointer    data)
{
  app_data * app = data;

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      g_print ("End of stream detected on bus\n");
      drop_bin (app);
      /*g_main_loop_quit (app->loop);*/
      break;

    case GST_MESSAGE_ERROR:
      {
        gchar  *debug;
        GError *error;

        gst_message_parse_error (msg, &error, &debug);

        g_printerr ("Error: %s\n", error->message);
        g_error_free (error);

        g_printerr ("Debugging info: %s\n", (debug) ? debug : "none");
        g_free (debug);

        g_main_loop_quit (app->loop);
        break;
      }
    default:
      break;
  }

  return TRUE;
}

GstElement * create_bin (char * output_location, app_data * app)
{
  g_printf ("Saving stream to %s...\n", output_location);

  GstElement *bin = gst_element_factory_make ("bin", NULL),
             *mux = gst_element_factory_make ("mp4mux", "mux"),
             *sink = gst_element_factory_make ("filesink", "sink");

  if (!bin) { g_printerr("Failed to create bin"); }
  if (!mux) { g_printerr("Failed to create mux"); }
  if (!sink) { g_printerr("Failed to create sink"); }

  if (!bin || !mux || !sink) {
    return NULL;
  }

  g_object_set (sink, "location", output_location, NULL);

  gst_bin_add_many (GST_BIN (bin), mux, sink, NULL);
  gst_element_link (mux, sink);
  gst_element_add_pad(bin, gst_ghost_pad_new ("sink", gst_element_get_request_pad (mux, "video_%u")));

  return bin;
}

static GstPadProbeReturn
blockpad_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  app_data * app = user_data;
  GstElement * mux = gst_bin_get_by_name (GST_BIN(app->bin), "mux");
  GstPad * mux_sink_pad = gst_element_get_static_pad (mux, "video_0");

  /*gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));*/

  gst_pad_send_event (mux_sink_pad, gst_event_new_eos());
  g_object_unref (mux_sink_pad);

  return GST_PAD_PROBE_OK;
}

void block_pipeline(app_data *app)
{
  app->blockpad_probe_id = gst_pad_add_probe (app->blockpad,
      GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER,
      blockpad_probe_cb, app, NULL);
}

static gint inside_window(GstPadProbeInfo * info, app_data * app, gboolean need_keyframe)
{
  GstClockTime pts = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info));
  GstBufferFlags flags = GST_BUFFER_FLAGS(GST_PAD_PROBE_INFO_BUFFER (info));
  gboolean have_keyframe = (flags & GST_BUFFER_FLAG_DELTA_UNIT) != GST_BUFFER_FLAG_DELTA_UNIT;

  gint ret = pts < app->clock_start ? -1 : pts >= app->clock_end ? 1 : 0;

  /*g_printf ("pts check : %lu%s %s [%lu, %lu]\n", pts, have_keyframe ? "*" : "",*/
      /*ret == -1 ? "before" : ret == 1 ? "after" : "inside",*/
      /*app->clock_start, app->clock_end);*/

  if (need_keyframe && !have_keyframe && ret == 0)
    ret = -1;

  return ret;
}

static GstPadProbeReturn
wait_for_end_cb (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  app_data * app = data;


  switch (inside_window(info, app, FALSE)) {
    case -1:
      g_printerr ("Somehow we're before our window looking for end");
      break;

    case 0:
      /*g_print ("Passing along a frame that is in window\n");*/
      return GST_PAD_PROBE_PASS;

    case 1:
    default:
      break;
  }


  gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));
  block_pipeline(app);

  return GST_PAD_PROBE_DROP;
}


static GstPadProbeReturn
wait_for_start_cb (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  app_data * app = data;

  switch (inside_window(info, app, TRUE)) {
    case -1:
      /*g_printf ("Dropping a frame that is too old\n");*/
      return GST_PAD_PROBE_DROP;
    case 0:
      /*g_print ("Found a key frame that is in range\n");*/

      app->clock_end = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info)) + app->clock_desired_duration;

      gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));

      gst_pad_add_probe (app->blockpad,
          GST_PAD_PROBE_TYPE_BUFFER,
          wait_for_end_cb, app, NULL);

      /*g_print ("Scanning for end frame\n");*/
      return GST_PAD_PROBE_PASS;
    default:
    case 1:
      g_printerr ("Didn't find a keyframe in range!\n");
      gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));
      block_pipeline(app);
      return GST_PAD_PROBE_DROP;
  }
}


void unblock_pipeline(app_data *app)
{
  char file_name[128];

  g_sprintf (file_name, "test_%u.mp4", app->file_count++);
  app->bin = create_bin (file_name, app);
  gst_bin_add (GST_BIN(app->pipeline), app->bin);
  gst_element_link (app->queue2, app->bin);
  gst_element_set_state (app->bin, GST_STATE_PLAYING);

  gst_pad_add_probe (app->blockpad,
      GST_PAD_PROBE_TYPE_BUFFER,
      wait_for_start_cb, app, NULL);

  if (app->blockpad_probe_id) {
    /*g_print ("Unblocking pipeline\n");*/
    gst_pad_remove_probe (app->blockpad, app->blockpad_probe_id);
    app->blockpad_probe_id = 0;
  }
  /*g_print ("Scanning for key frame\n");*/
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
  app_data * app = data;

  switch (g_io_channel_read_line_string(source, buffer, NULL, &error)) {
    case G_IO_STATUS_NORMAL:
      g_strstrip(buffer->str);
      if (!strcmp (buffer->str, ""))
        break;
      g_print("received command %s\n", buffer->str);
      if (!strcmp("start", buffer->str)) {
        unblock_pipeline(app);
        /*unblock_pipeline(app, file_name);*/
      } else if (!strcmp("stop", buffer->str)) {
        block_pipeline(app);
      } else if (!strcmp("pause", buffer->str)) {
        set_state_and_wait (app->pipeline, GST_STATE_PAUSED, app);
      } else if (!strcmp("shutdown", buffer->str)) {
        g_print("Shutting down\n");
        g_main_loop_quit (app->loop);
      } else if (!strcmp("query", buffer->str)) {
        GstClockTime level_time;
        guint level_bytes;
        guint level_buffers;

        g_object_get (app->queue1,
            "current-level-time", &level_time,
            "current-level-bytes", &level_bytes,
            "current-level-buffers", &level_buffers, NULL);

        g_printf("queue1 reports %lums, %u buffers, %u bytes\n",
            GST_TIME_AS_MSECONDS(level_time), level_buffers, level_bytes);

        g_object_get (app->queue2,
            "current-level-time", &level_time,
            "current-level-bytes", &level_bytes,
            "current-level-buffers", &level_buffers, NULL);

        g_printf("queue2 reports %lums, %u buffers, %u bytes\n",
            GST_TIME_AS_MSECONDS(level_time), level_buffers, level_bytes);
      } else if (g_str_has_prefix(buffer->str, "replay ")) {
        GstClockTime current_duration;
        glong relative_start;
        glong duration;
        gchar * next = &buffer->str[7];

        relative_start = strtol(next, &next, 10);
        duration = strtol(next, &next, 10);

        current_duration = GST_CLOCK_DIFF(app->base_time, get_current_time());

        if (-relative_start * GST_SECOND > current_duration)
          relative_start = -GST_TIME_AS_SECONDS(current_duration);

        /*g_print ("%20lu: get_current_time()\n",  get_current_time());*/
        /*g_print ("%20lu: base_time\n", app->base_time);*/
        /*g_print ("%20lu: get_current_time - base_time\n", get_current_time() - app->base_time);*/
        /*g_print ("%20ld: relative_start * GST_SECOND\n", relative_start * GST_SECOND);*/

        app->clock_start = current_duration + relative_start * GST_SECOND;
        app->clock_desired_duration = duration * GST_SECOND;
        app->clock_end = app->clock_start + app->clock_desired_duration;

        /*g_print ("replaying with %lu and %lu\n", GST_TIME_AS_MSECONDS(app->clock_start), GST_TIME_AS_MSECONDS(app->clock_desired_duration));*/
        unblock_pipeline(app);
      } else {
        g_print("Unrecognized command\n");
      }
      break;
    case G_IO_STATUS_ERROR:
      g_print("no!!! error");
      g_error_free (error);
      break;
    case G_IO_STATUS_EOF:
      g_print("EOF reached\n");
      break;
    case G_IO_STATUS_AGAIN:
      return TRUE;
    default:
      g_return_val_if_reached(FALSE);
      break;
  }
  return TRUE;
}

int
main (int   argc,
    char *argv[])
{
  GstBus *bus;
  guint bus_watch_id;
  app_data * app = &global_app_data;
  GOptionContext * option_context;
  GError * error;
  char * control_pipe = "/dev/stdin";

  GOptionEntry option_entries[] = {
    { "pipe", 'p', 0, G_OPTION_ARG_STRING, &control_pipe, "Pathname of control pipe", "PIPE" },
    { NULL }
  };

  /* Initialisation */
  gst_init (&argc, &argv);

  option_context = g_option_context_new ("- start queue-buffered video server");
  g_option_context_add_main_entries (option_context, option_entries, NULL);
  g_option_context_parse (option_context, &argc, &argv, &error);
  g_option_context_free (option_context);

  app->status = STATUS_IDLE;
  app->file_count = 0;
  app->loop = g_main_loop_new (NULL, FALSE);
  app->is_blocking = TRUE;
  app->blockpad_probe_id = 0;

  /* Create gstreamer elements */
  app->pipeline  = gst_pipeline_new ("camsrc");
  app->source    = gst_element_factory_make ("videotestsrc", "video-source");
  app->caps      = gst_element_factory_make ("capsfilter", "caps-filter");
  app->videorate = gst_element_factory_make ("videorate", "video-rate");
  app->converter = gst_element_factory_make ("videoconvert", "video-convert");
  app->queue1    = gst_element_factory_make ("queue", "upstream-queue");
  app->encoder   = gst_element_factory_make ("x264enc", "video-encoder");
  app->queue2    = gst_element_factory_make ("queue", "ringbuffer-queue");
  app->bin       = create_bin ("/dev/null", app);

  if (!app->pipeline) { g_printerr ("Failed to create pipeline"); }
  if (!app->source) { g_printerr("failed to create videotestsrc"); }
  if (!app->caps) { g_printerr ("Failed to create capsfilter"); }
  if (!app->videorate) { g_printerr("failed to create video-rate"); }
  if (!app->converter) { g_printerr("failed to create videoconvert"); }
  if (!app->queue1) { g_printerr("failed to create upstream-queue"); }
  if (!app->encoder) { g_printerr("failed to create encoder"); }
  if (!app->queue2) { g_printerr("failed to create ringbuffer-queue"); }

  if (!app->pipeline ||
      !app->source ||
      !app->caps ||
      !app->videorate ||
      !app->converter ||
      !app->queue1 ||
      !app->encoder ||
      !app->queue2 ||
      !app->bin) {
    g_printerr ("An element could not be created. Exiting.\n");
    return -1;
  }

#define GST_QUEUE_LEAK_DOWNSTREAM 2
  g_object_set (app->source, "is-live", TRUE, NULL);
  g_object_set (app->source, "pattern", 18, NULL);
  g_object_set (app->queue2,
      "leaky", GST_QUEUE_LEAK_DOWNSTREAM,
      "max-size-bytes", 500 * 1024 * 1024,
      "max-size-buffers", 0,
      "max-size-time", 0,
      NULL);
  g_object_set (app->encoder, "byte-stream", TRUE, NULL);
  g_object_set (app->encoder, "key-int-max", 30, NULL);

  GstCaps *caps;
  caps = gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "I420",
      "framerate", GST_TYPE_FRACTION, 30, 1,
      "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
      "width", G_TYPE_INT, 1920,
      "height", G_TYPE_INT, 1080,
      NULL);

  g_object_set (app->caps, "caps", caps, NULL);

  /* Add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, app);
  gst_object_unref (bus);

  /* Add elements up to the second queue -- the "fixed" part of the pipeline */
  gst_bin_add_many (GST_BIN (app->pipeline),
      app->source, app->caps, app->videorate, app->converter, app->queue1, app->encoder, app->queue2, app->bin, NULL);

  /* we link the elements together */
  gst_element_link_many (
      app->source, app->caps, app->videorate, app->converter, app->queue1, app->encoder, app->queue2, app->bin, NULL);

  app->blockpad = gst_element_get_static_pad (app->queue2, "src");

  block_pipeline(app);

   /*Verbose*/
  /*g_signal_connect (app->pipeline, "deep-notify",*/
    /*G_CALLBACK (gst_object_default_deep_notify), NULL);*/

  /* Set the pipeline to "playing" state */
  gst_element_set_state (app->pipeline, GST_STATE_PLAYING);

  app->base_time = get_current_time();

  GIOChannel *io = NULL;
  guint io_watch_id = 0;

  /* set up control pipe */
  mkfifo(control_pipe, 0666);
  int pipe = g_open(control_pipe, O_RDONLY | O_NONBLOCK, 0);
  io = g_io_channel_unix_new (pipe);
  io_watch_id = g_io_add_watch (io, G_IO_IN, io_callback, app);
  g_io_channel_unref (io);

  /* Iterate */
  g_printf ("Listening on %s...\n", control_pipe);

  g_main_loop_run (app->loop);

  /* Out of the main loop, clean up nicely */
  gst_element_set_state (app->pipeline, GST_STATE_NULL);

  gst_object_unref (GST_OBJECT (app->pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (app->loop);
  unlink(control_pipe);

  return 0;
}
