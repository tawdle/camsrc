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
  GstElement *videorate;
  GstElement *converter;
  GstElement *queue1;
  GstElement *encoder;
  GstElement *queue2;
  GstElement *bin;
  GstElement *mux;
  GstElement *sink;
  app_status status;
  GstPad * blockpad;
  gulong block_probe_id;
  guint file_count;
  gboolean first;
  gboolean blocking;
} app_data;

app_data global_app_data;

void unblock_pipeline(app_data *app, char * output_location);

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
  GstElement * old_bin = app->bin;

  g_printf ("Unlinking bin...\n");
  gst_element_unlink (app->queue2, old_bin);

  g_print ("Unblocking pipeline...\n");

  // Send a few frames to the bit bucket...
  unblock_pipeline (app, "/dev/null");

  g_printf ("set_state_and_wait\n");
  gst_element_set_state (app->bin, GST_STATE_NULL);
  /*set_state_and_wait(app->bin, GST_STATE_NULL, app);*/
  g_printf ("removing bin\n");
  gst_bin_remove (GST_BIN(app->pipeline), app->bin);
  g_printf("done!\n");
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
      g_main_loop_quit (app->loop);
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


static GstPadProbeReturn
blockpad_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  g_printf ("pad is blocked now\n");
  GST_DEBUG_OBJECT (pad, "pad is blocked now");

  // Can we send to the element like this?
  // gst_element_send_event (app->mux, gst_event_new_eos());

  GstPad * peer = gst_pad_get_peer (pad);
  gst_pad_send_event (peer, gst_event_new_eos ());
  gst_object_unref (peer);

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

static void
gst_bin_handle_message_func (GstBin * bin, GstMessage * message)
{
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
    drop_bin(&global_app_data);
  }
}

void unblock_pipeline(app_data *app, char * output_location)
{
  g_printf ("Saving stream to %s...\n", output_location);

  app->bin = gst_element_factory_make ("bin", NULL);
  app->mux = gst_element_factory_make ("mp4mux", "mux");
  app->sink = gst_element_factory_make ("filesink", "sink");

  if (!app->bin) { g_printerr("Failed to create bin"); }
  if (!app->mux) { g_printerr("Failed to create mux"); }
  if (!app->sink) { g_printerr("Failed to create sink"); }

  if (!app->bin || !app->mux || !app->sink) {
    g_main_loop_quit (app->loop);
  }

  g_object_set (app->sink, "location", output_location, NULL);

  gst_bin_add_many (GST_BIN (app->bin), app->mux, app->sink, NULL);

  gst_element_link (app->mux, app->sink);

  gst_element_add_pad(app->bin, gst_ghost_pad_new ("sink", gst_element_get_request_pad (app->mux, "video_%u")));

  gst_bin_add (GST_BIN(app->pipeline), app->bin);

  gst_element_link (app->queue2, app->bin);

  gst_element_sync_state_with_parent (app->bin);

  GstBinClass * bin_class = GST_BIN_GET_CLASS(app->bin);
  bin_class->handle_message = gst_bin_handle_message_func;

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
      } else if (!strcmp("pause", buffer->str)) {
        set_state_and_wait (app->pipeline, GST_STATE_PAUSED, app);
      } else if (!strcmp("shutdown", buffer->str)) {
        g_print("Shutting down\n");
        gst_element_send_event (app->pipeline, gst_event_new_eos ());
        /*g_main_loop_quit (app->loop);*/
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

#if 0
static gboolean
timeout_cb (gpointer user_data)
{
  app_data * app = user_data;

  gst_pad_add_probe (app->blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
      blockpad_probe_cb, user_data, NULL);

  return FALSE;
}
#endif

int
main (int   argc,
    char *argv[])
{
  GstBus *bus;
  guint bus_watch_id;
  app_data * app = &global_app_data;

  /* Initialisation */
  gst_init (&argc, &argv);

  app->status = STATUS_IDLE;
  app->block_probe_id = 0;
  app->file_count = 0;
  app->sink = NULL;
  app->mux = NULL;
  app->loop = g_main_loop_new (NULL, FALSE);
  app->first = TRUE;

  /* Check input arguments */
  /*if (argc != 2) {*/
    /*g_printerr ("Usage: %s <Ogg/Vorbis filename>\n", argv[0]);*/
    /*return -1;*/
  /*}*/


  /* Create gstreamer elements */
  app->pipeline  = gst_pipeline_new ("gerald");
  app->source    = gst_element_factory_make ("videotestsrc", "video-source");
  app->videorate = gst_element_factory_make ("videorate", "video-rate");
  app->converter = gst_element_factory_make ("videoconvert", "video-convert");
  app->queue1    = gst_element_factory_make ("queue", "upstream-queue");
  app->encoder   = gst_element_factory_make ("x264enc", "video-encoder");
  app->queue2    = gst_element_factory_make ("queue", "ringbuffer-queue");

  if (!app->pipeline) { g_printerr ("Failed to create pipeline"); }
  if (!app->source) { g_printerr("failed to create videotestsrc"); }
  if (!app->videorate) { g_printerr("failed to create video-rate"); }
  if (!app->converter) { g_printerr("failed to create videoconvert"); }
  if (!app->queue1) { g_printerr("failed to create upstream-queue"); }
  if (!app->encoder) { g_printerr("failed to create encoder"); }
  if (!app->queue2) { g_printerr("failed to create ringbuffer-queue"); }

  if (!app->pipeline ||
      !app->source ||
      !app->videorate ||
      !app->converter ||
      !app->queue1 ||
      !app->encoder ||
      !app->queue2) {
    g_printerr ("An element could not be created. Exiting.\n");
    return -1;
  }

  g_print ("Elements added, setting props\n");

#define GST_QUEUE_LEAK_DOWNSTREAM 2
  g_object_set (app->source, "is-live", TRUE, NULL);
  g_object_set (app->queue2, "max-size-bytes", 10 * 1024 * 1024, NULL);
  g_object_set (app->queue2, "leaky", GST_QUEUE_LEAK_DOWNSTREAM, NULL);

  /* Set up the pipeline */

  /* Add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (app->pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, app);
  gst_object_unref (bus);

  g_print("Adding elements...\n");
  /* Add elements up to the second queue -- the "fixed" part of the pipeline */
  gst_bin_add_many (GST_BIN (app->pipeline),
      app->source, app->videorate, app->converter, app->queue1, app->encoder, app->queue2, NULL);

  g_print("Linking elements...\n");
  /* we link the elements together */
  gst_element_link_many (
      app->source, app->videorate, app->converter, app->queue1, app->encoder, app->queue2, NULL);

  app->blockpad = gst_element_get_static_pad (app->queue2, "src");

  unblock_pipeline (app, "first.mp4");

  // Verbose
   g_signal_connect (app->pipeline, "deep-notify",
    G_CALLBACK (gst_object_default_deep_notify), NULL);

  /* Set the pipeline to "playing" state*/
  g_print ("Starting pipeline...\n");
  set_state_and_wait (app->pipeline, GST_STATE_PLAYING, app);

  /*block_pipeline (&app);*/

  GIOChannel *io = NULL;
  guint io_watch_id = 0;

  /* set up control pipe */
  char * control_pipe = "/dev/stdin";
  mkfifo(control_pipe, 0666);
  int pipe = g_open(control_pipe, O_RDONLY, O_NONBLOCK);
  io = g_io_channel_unix_new (pipe);
  io_watch_id = g_io_add_watch (io, G_IO_IN, io_callback, app);
  g_io_channel_unref (io);

  /* Iterate */
  g_print ("Running...\n");
  g_main_loop_run (app->loop);

  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (app->pipeline, GST_STATE_NULL);

  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (app->pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (app->loop);
  unlink(control_pipe);

  return 0;
}
