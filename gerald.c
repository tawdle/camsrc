#include <gst/gst.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>

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
        g_free (debug);

        g_printerr ("Error: %s\n", error->message);
        g_error_free (error);

        g_main_loop_quit (loop);
        break;
      }
    default:
                            break;
  }

  return TRUE;
}


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

static GstPadProbeReturn
blockpad_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);
  GstClockTime pts = GST_BUFFER_PTS(buffer);
  GstClockTime dts = GST_BUFFER_DTS(buffer);

  /*g_print ("blockpad_probe_cb: pts = %ld dts = %ld\n", pts, dts);*/

  GST_DEBUG_OBJECT (pad, "pad is blocked now");

  return GST_PAD_PROBE_DROP;
}


gboolean io_callback(GIOChannel *source, GIOCondition condition, gpointer data)
{
  GError *error = NULL;
  GString *buffer = g_string_new(NULL);

  g_print("Entering io_callback...\n");

  switch (g_io_channel_read_line_string(source, buffer, NULL, &error)) {
    case G_IO_STATUS_NORMAL:
      g_print("got it! line is %s", buffer->str);
      break;
    case G_IO_STATUS_ERROR:
      g_print("no!!! error");
      g_error_free (error);
      break;
    case G_IO_STATUS_EOF:
      g_print("EOF reached");
      break;
    case G_IO_STATUS_AGAIN:
      return TRUE;
    default:
      g_return_val_if_reached(FALSE);
      break;
  }
  g_print("leaving io_callback\n");
  return TRUE;
}


int
main (int   argc,
    char *argv[])
{
  GMainLoop *loop;

  GstElement *pipeline, *source, *converter, *queue1, *encoder, *queue2;
  GstPad *blockpad;
  GstBus *bus;
  guint bus_watch_id;

  /* Initialisation */
  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);


  /* Check input arguments */
  /*if (argc != 2) {*/
    /*g_printerr ("Usage: %s <Ogg/Vorbis filename>\n", argv[0]);*/
    /*return -1;*/
  /*}*/


  /* Create gstreamer elements */
  pipeline  = gst_pipeline_new ("gerald");
  source    = gst_element_factory_make ("videotestsrc", "video-source");
  converter = gst_element_factory_make ("videoconvert", "video-convert");
  queue1    = gst_element_factory_make ("queue", "queue1");
  encoder   = gst_element_factory_make ("x264enc", "video-encoder");
  queue2    = gst_element_factory_make ("queue", "queue2");

  if (!pipeline) { g_printerr ("Failed to create pipeline"); }
  if (!source) { g_printerr("failed to create videotestsrc"); }
  if (!converter) { g_printerr("failed to create videoconvert"); }
  if (!queue1) { g_printerr("failed to create queue1"); }
  if (!encoder) { g_printerr("failed to create encoder"); }
  if (!queue2) { g_printerr("failed to create queue2"); }

  if (!pipeline || !converter || !queue1 || !encoder || !queue2) {
    g_printerr ("One element could not be created. Exiting.\n");
    return -1;
  }

  /* Set up the pipeline */

  /* we set the input filename to the source element */
  /*g_object_set (G_OBJECT (source), "location", argv[1], NULL);*/

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
  gst_object_unref (bus);

  /* we add all elements into the pipeline */
  gst_bin_add_many (GST_BIN (pipeline),
      source, converter, queue1, encoder, queue2, NULL);

  /* we link the elements together */
  gst_element_link_many (
      source, converter, queue1, encoder, queue2, NULL);

  /* add a blocking probe on queue2's source pad */
  blockpad = gst_element_get_static_pad (queue2, "src");
  gst_pad_add_probe (blockpad, GST_PAD_PROBE_TYPE_BUFFER,
      blockpad_probe_cb, loop, NULL);

  /* Set the pipeline to "playing" state*/
  g_print ("Starting pipeline...\n");
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  GIOChannel *io = NULL;
  guint io_watch_id = 0;

  /* set up control pipe */
  char * control_pipe = "/tmp/gerald";
  mkfifo(control_pipe, 0666);
  int pipe = g_open(control_pipe, O_RDONLY, O_NONBLOCK);
  io = g_io_channel_unix_new (pipe);
  io_watch_id = g_io_add_watch (io, G_IO_IN, io_callback, NULL);
  g_io_channel_unref (io);

  /* Iterate */
  g_print ("Running...\n");
  g_main_loop_run (loop);

  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);

  g_print ("Deleting pipeline\n");
  gst_object_unref (GST_OBJECT (pipeline));
  g_source_remove (bus_watch_id);
  g_main_loop_unref (loop);
  unlink(control_pipe);

  return 0;
}

/*

/bin/sh ~/gst/master/gstreamer/libtool --mode=link gcc -Wall gerald.c -o gerald $(pkg-config --cflags --libs gstreamer-1.0)

*/
