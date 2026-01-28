#include <gst/gst.h>
#include <glib.h>
#include <csignal>
#include <atomic>
#include <iostream>
#include <string>

/* =======================
 * Globals
 * ======================= */
static GMainLoop *loop = nullptr;
static GstElement *pipeline = nullptr;

static std::atomic<uint64_t> frame_count{0};
static uint64_t last_frame_count = 0;

static const char *current_stage = "NULL";
static int state_tick = 0;
static guint state_timer_id = 0;

/* =======================
 * Forward declarations
 * ======================= */
static GstElement* create_pipeline(const std::string &host, int port);
static gboolean state_machine_cb(gpointer);

/* =======================
 * Signal handling
 * ======================= */
static void handle_sigint(int) {
    std::cerr << "\n[SIGNAL] SIGINT received, stopping...\n";
    if (loop)
        g_main_loop_quit(loop);
}

/* =======================
 * FPS handoff callback
 * ======================= */
static void on_handoff(GstElement *, GstBuffer *, GstPad *, gpointer) {
    frame_count++;
}

/* =======================
 * FPS logger (every 5s)
 * ======================= */
static gboolean fps_log_cb(gpointer) {
    uint64_t now = frame_count.load();
    uint64_t diff = now - last_frame_count;
    last_frame_count = now;

    double fps = diff / 5.0;

    std::cerr << "[FPS] stage=" << current_stage
              << " fps=" << fps << std::endl;
    return TRUE;
}

/* =======================
 * State helper
 * ======================= */
static void set_pipeline_state(GstState state, const char *name) {
    current_stage = name;
    std::cerr << "[STATE] -> " << name << std::endl;
    gst_element_set_state(pipeline, state);
}

/* =======================
 * State machine
 * ======================= */
enum class Phase {
    PLAYING,
    PAUSED,
    READY,
    NULL_STATE
};

static Phase phase = Phase::NULL_STATE;

/* -----------------------
 * Timer re-arm helper
 * ----------------------- */
static void arm_next_state_timer(guint seconds) {
    if (state_timer_id != 0) {
        g_source_remove(state_timer_id);
        state_timer_id = 0;
    }
    state_timer_id = g_timeout_add_seconds(seconds, state_machine_cb, nullptr);
}

/* -----------------------
 * State machine callback
 * ----------------------- */
static gboolean state_machine_cb(gpointer) {
    state_tick++;
    std::cerr << "[STATE_TICK] #" << state_tick << std::endl;

    switch (phase) {
        case Phase::NULL_STATE:
            set_pipeline_state(GST_STATE_PLAYING, "PLAYING");
            phase = Phase::PLAYING;

            // PLAYING for 30 seconds
            arm_next_state_timer(10);
            break;

        case Phase::PLAYING:
            set_pipeline_state(GST_STATE_PAUSED, "PAUSED");
            phase = Phase::PAUSED;

            // PAUSED for 1 second
            arm_next_state_timer(1);
            break;

        case Phase::PAUSED:
            set_pipeline_state(GST_STATE_READY, "READY");
            phase = Phase::READY;

            // NULL for 1 second
            arm_next_state_timer(1);
            break;
        
        case Phase::READY:
            set_pipeline_state(GST_STATE_NULL, "NULL");
            gst_object_unref(pipeline);

            pipeline = create_pipeline("10.0.0.2", 5000);
            phase = Phase::NULL_STATE;

            // PAUSED for 1 second
            arm_next_state_timer(1);
            break;
    }

    return G_SOURCE_REMOVE; // one-shot timer
}

/* =======================
 * Tee → queue helper
 * ======================= */
static bool link_tee_to_queue(GstElement *tee, GstElement *queue) {
    GstPad *tee_src = gst_element_request_pad_simple(tee, "src_%u");
    GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");

    if (!tee_src || !queue_sink) {
        if (tee_src) gst_object_unref(tee_src);
        if (queue_sink) gst_object_unref(queue_sink);
        return false;
    }

    GstPadLinkReturn ret = gst_pad_link(tee_src, queue_sink);
    gst_object_unref(tee_src);
    gst_object_unref(queue_sink);

    return ret == GST_PAD_LINK_OK;
}

/* =======================
 * Pipeline creation
 * ======================= */
static GstElement* create_pipeline(const std::string &host, int port) {
    GError *error = nullptr;

    std::string base =
        "hailofrontendbinsrc config-file-path=/home/root/apps/detection/resources/configs/frontend_config.json name=frontend "
        "frontend. ! queue ! "
        "hailonet hef-path=/home/root/apps/detection/resources/yolov5m_wo_spp_60p_nv12_fhd.hef "
        "scheduling-algorithm=1 vdevice-group-id=device0 ! queue ! "
        "hailofilter function-name=yolov5 "
        "config-path=/home/root/apps/detection/resources/configs/yolov5.json "
        "so-path=/usr/lib/hailo-post-processes/libyolo_post.so qos=false ! queue ! "
        "hailooverlay qos=false ! queue ! "
        "hailoencodebin config-file-path=/home/root/apps/detection/resources/configs/encoder_config.json ! "
        "h264parse name=parser config-interval=-1 ! "
        "video/x-h264,framerate=30/1";

    GstElement *pipe = gst_parse_launch(base.c_str(), &error);
    if (!pipe) {
        if (error) {
            std::cerr << error->message << std::endl;
            g_error_free(error);
        }
        return nullptr;
    }
    if (error) g_error_free(error);

    GstElement *tee  = gst_element_factory_make("tee", nullptr);
    GstElement *q1   = gst_element_factory_make("queue", nullptr);
    GstElement *pay  = gst_element_factory_make("rtph264pay", nullptr);
    GstElement *udp  = gst_element_factory_make("udpsink", nullptr);
    GstElement *q2   = gst_element_factory_make("queue", nullptr);
    GstElement *id   = gst_element_factory_make("identity", nullptr);
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);

    g_object_set(udp, "host", host.c_str(), "port", port, "sync", FALSE, nullptr);
    g_object_set(id, "signal-handoffs", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(pipe),
                     tee, q1, pay, udp,
                     q2, id, sink,
                     nullptr);

    GstElement *parser = gst_bin_get_by_name(GST_BIN(pipe), "parser");
    gst_element_link(parser, tee);
    gst_object_unref(parser);

    link_tee_to_queue(tee, q1);
    gst_element_link_many(q1, pay, udp, nullptr);

    link_tee_to_queue(tee, q2);
    gst_element_link_many(q2, id, sink, nullptr);

    g_signal_connect(id, "handoff", G_CALLBACK(on_handoff), nullptr);
    return pipe;
}

/* =======================
 * Main
 * ======================= */
int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    std::signal(SIGINT, handle_sigint);

    pipeline = create_pipeline("10.0.0.2", 5000);
    if (!pipeline) return -1;

    loop = g_main_loop_new(nullptr, FALSE);

    g_timeout_add_seconds(5, fps_log_cb, nullptr);
    arm_next_state_timer(1); // start state machine

    std::cerr << "[INFO] PLAYING=30s, PAUSED=1s, NULL=1s (recreate)\n";
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
