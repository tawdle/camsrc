/*
 We can't successfully get caps neogotiated unless the pipeline is complete when we start it.
 So here's what we do:

 At the start, we put everything together, pointing filesink at /dev/null.
 We set the pipeline state to playing; when we detect that the pipeline actually is playing,
 we shut down the back-end.

 To do the shut-down, we follow the recommeneded technique:

 * block the srcpad on queue2
 * unlink the mux from queue2
 * set an event probe on mux's srcpad
 * send an EOS on mux's sink pad
 * when we receive the EOS on the probe on mux's srcpad, we remove the probe and drop the EOS
 * then we set the states on mux and sink to NULL
 * we may need to wait for the sink to go to NULL
 * when it does, we remove it
 *
 * To start a save, we:
 * create a mux and a sink, point the sink at the right fd
 * add the mux and sink to the pipeline
 * link queue2 => mux
 * set states of mux and sink to PLAYING
 * remove the block on queue2's srcpad
 * */

#include <gst/gst.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

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
  GstElement *converter;
  GstElement *queue1;
  GstElement *encoder;
  GstElement *queue2;
  GstElement *mux;
  GstElement *sink;
  app_status status;
  GstPad * blockpad;
  gulong block_probe_id;
  guint file_count;
  gboolean first;
} app_data;

static gboolean
bus_call (GstBus     *bus,
    GstMessage *msg,
    gpointer    data)
{
  GMainLoop *loop = (GMainLoop *) data;

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
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

        g_main_loop_quit (loop);
        break;
      }
    default:
      break;
  }

  return TRUE;
}


void set_state_and_wait (GstElement * element, GstState state, app_data * app)
{
  GstState new_state;

  switch (gst_element_set_state (element, state)) {
    case GST_STATE_CHANGE_SUCCESS:
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
      break;
  }
}

#if 0
static void
on_pad_added (GstElement *element,
    GstPad     *pad,
    gpointer    data)
{
  GstPad *sinkpad;
  GstElement *decoder = (GstElement *) data;

  /* We can now link this pad with the vorbis-decoder sink pad */
  g_print ("Dynamic pad created, linking demuxer/decoder\n");

  sinkpad = gst_element_get_static_pad (decoder, "sink");

  gst_pad_link (pad, sinkpad);

  gst_object_unref (sinkpad);
}
#endif

void drop_mux_and_sink (app_data * app)
{
  gst_element_set_state (app->mux, GST_STATE_NULL);

  g_printf("setting sink to NULL\n");

  set_state_and_wait (app->sink, GST_STATE_NULL, app);

  g_printf ("removing mux and sink...\n");
  gst_bin_remove (GST_BIN(app->pipeline), app->mux);
  gst_bin_remove (GST_BIN(app->pipeline), app->sink);
  g_printf("done!\n");

}


static GstPadProbeReturn
eos_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  app_data * app = user_data;

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_DATA (info)) != GST_EVENT_EOS) {
    g_printf ("got another event that wasn't our EOS; ignoring\n");
    return GST_PAD_PROBE_OK;
  }

  g_printf("Received EOS event\n");

  gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));

  drop_mux_and_sink(app);

  return GST_PAD_PROBE_DROP;
}


static GstPadProbeReturn
blockpad_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  /*GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);*/
  /*GstClockTime pts = GST_BUFFER_PTS(buffer);*/
  /*GstClockTime dts = GST_BUFFER_DTS(buffer);*/

  /*g_print ("blockpad_probe_cb: pts = %ld dts = %ld\n", pts, dts);*/

  gst_pad_remove_probe (pad, GST_PAD_PROBE_INFO_ID (info));

  app_data * app = user_data;

  GST_DEBUG_OBJECT (pad, "pad is blocked now");

  g_printf ("Unlinking mux...\n");
  gst_element_unlink (app->queue2, app->mux);

  g_printf ("Pausing queue...\n");
  set_state_and_wait ( app->queue2, GST_STATE_PAUSED, app);

  g_printf ("Setting event probe...\n");
  GstPad * srcpad = gst_element_get_static_pad (app->mux, "src");
  gst_pad_add_probe (srcpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, eos_probe_cb, app, NULL);
  gst_object_unref (srcpad);

  g_printf ("Getting sinkpad... ");
  GstPad * sinkpad = gst_element_get_static_pad (app->mux, "video_0");
  g_printf ("sinkpad is %p\n", sinkpad);

  g_printf ("Generating new EOS... ");
  GstEvent * event = gst_event_new_eos ();
  g_printf ("event is %p\n", event);

  g_printf ("Sending event...\n");
  gst_pad_send_event (sinkpad, event);
  gst_object_unref (sinkpad);

  g_printf("Sent! Now we wait.\n");
  return GST_PAD_PROBE_OK;
}

void block_pipeline(app_data *app)
{
  g_print ("Blocking pipeline...\n");
  g_printf ("Adding blocking probe...\n");
  app->block_probe_id = gst_pad_add_probe (
      app->blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
      blockpad_probe_cb, app, NULL);

}

void unblock_pipeline(app_data *app, char * output_location)
{
  g_printf ("Saving stream to %s...\n", output_location);

  app->mux = gst_element_factory_make ("mp4mux", "mux");
  app->sink = gst_element_factory_make ("filesink", "sink");

  if (!app->mux) { g_printerr("Failed to create mux"); }
  if (!app->sink) { g_printerr("Failed to create sink"); }

  if (!app->mux || !app->sink) {
    g_main_loop_quit (app->loop);
  }

  g_object_set (app->sink, "location", output_location, NULL);
  gst_bin_add_many (GST_BIN (app->pipeline), app->mux, app->sink, NULL);
  gst_element_link_many (app->queue2, app->mux, app->sink, NULL);

  gst_element_sync_state_with_parent (app->mux);
  gst_element_sync_state_with_parent (app->sink);

  if (app->block_probe_id) {
    gst_pad_remove_probe (app->blockpad, app->block_probe_id);
    app->block_probe_id = 0;
  }
}

gboolean io_callback(GIOChannel *source, GIOCondition condition, gpointer data)
{
  GError *error = NULL;
  GString *buffer = g_string_new(NULL);
  app_data * app = data;
  char file_name[64];

  switch (g_io_channel_read_line_string(source, buffer, NULL, &error)) {
    case G_IO_STATUS_NORMAL:
      g_strstrip(buffer->str);
      g_print("received command %s\n", buffer->str);
      if (!strcmp("start", buffer->str)) {
        g_sprintf (file_name, "test_%u.mp4", app->file_count++);
        unblock_pipeline(app, file_name);
      } else if (!strcmp("stop", buffer->str)) {
        block_pipeline(app);
      } else if (!strcmp("shutdown", buffer->str)) {
        g_print("Shutting down\n");
        g_main_loop_quit (app->loop);
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

static gboolean
timeout_cb (gpointer user_data)
{
  app_data * app = user_data;

  gst_pad_add_probe (app->blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
      blockpad_probe_cb, user_data, NULL);

  return FALSE;
}

int
main (int   argc,
    char *argv[])
{
  GstBus *bus;
  guint bus_watch_id;
  app_data app;

  /* Initialisation */
  gst_init (&argc, &argv);

  app.status = STATUS_IDLE;
  app.block_probe_id = 0;
  app.file_count = 0;
  app.sink = NULL;
  app.mux = NULL;
  app.loop = g_main_loop_new (NULL, FALSE);
  app.first = TRUE;

  /* Check input arguments */
  /*if (argc != 2) {*/
    /*g_printerr ("Usage: %s <Ogg/Vorbis filename>\n", argv[0]);*/
    /*return -1;*/
  /*}*/


  /* Create gstreamer elements */
  app.pipeline  = gst_pipeline_new ("gerald");
  app.source    = gst_element_factory_make ("videotestsrc", "video-source");
  app.converter = gst_element_factory_make ("videoconvert", "video-convert");
  app.queue1    = gst_element_factory_make ("queue", "upstream-queue");
  app.encoder   = gst_element_factory_make ("x264enc", "video-encoder");
  app.queue2    = gst_element_factory_make ("queue", "downstream-queue");

  if (!app.pipeline) { g_printerr ("Failed to create pipeline"); }
  if (!app.source) { g_printerr("failed to create videotestsrc"); }
  if (!app.converter) { g_printerr("failed to create videoconvert"); }
  if (!app.queue1) { g_printerr("failed to create upstream-queue"); }
  if (!app.encoder) { g_printerr("failed to create encoder"); }
  if (!app.queue2) { g_printerr("failed to create downstream-queue"); }

  if (!app.pipeline || !app.source || !app.converter || !app.queue1 || !app.encoder || !app.queue2) {
    g_printerr ("An element could not be created. Exiting.\n");
    return -1;
  }

  g_print ("Elements added, setting props\n");

#define GST_QUEUE_LEAK_DOWNSTREAM 2
  g_object_set (app.queue2, "max-size-bytes", 1024 * 1024 * 1024, NULL);
  g_object_set (app.queue2, "leaky", GST_QUEUE_LEAK_DOWNSTREAM, NULL);

  /* Set up the pipeline */

  /* we set the input filename to the source element */
  /*g_object_set (G_OBJECT (source), "location", argv[1], NULL);*/

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (app.pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, app.loop);
  gst_object_unref (bus);

  g_print("Adding elements...\n");
  /* we add all elements into the pipeline */
  gst_bin_add_many (GST_BIN (app.pipeline),
      app.source, app.converter, app.queue1, app.encoder, app.queue2, NULL);

  g_print("Linking elements...\n");
  /* we link the elements together */
  gst_element_link_many (
      app.source, app.converter, app.queue1, app.encoder, app.queue2, NULL);

  app.blockpad = gst_element_get_static_pad (app.queue2, "src");

  unblock_pipeline (&app, "/dev/null");

  // Verbose
  // g_signal_connect (app.pipeline, "deep-notify",
  //  G_CALLBACK (gst_object_default_deep_notify), NULL);

  /* Set the pipeline to "playing" state*/
  g_print ("Starting pipeline...\n");
  set_state_and_wait (app.pipeline, GST_STATE_PLAYING, &app);

  /*block_pipeline (&app);*/

  GIOChannel *io = NULL;
  guint io_watch_id = 0;

  /* set up control pipe */
  char * control_pipe = "/dev/stdin";
  mkfifo(control_pipe, 0666);
  int pipe = g_open(control_pipe, O_RDONLY, O_NONBLOCK);
  io = g_io_channel_unix_new (pipe);
  io_watch_id = g_io_add_watch (io, G_IO_IN, io_callback, &app);
  g_io_channel_unref (io);

  g_timeout_add_seconds (5, timeout_cb, app.loop);

  /* Iterate */
  g_print ("Running...\n");
  g_main_loop_run (app.loop);

  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (app.pipeline, GST_STATE_NULL);

  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (app.pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (app.loop);
  unlink(control_pipe);

  return 0;
}

/*

/bin/sh ~/gst/master/gstreamer/libtool --mode=link gcc -Wall gerald.c -o gerald $(pkg-config --cflags --libs gstreamer-1.0)

*/
