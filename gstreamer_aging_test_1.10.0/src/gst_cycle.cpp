#include <gst/gst.h>
#include <glib.h>
#include <csignal>
#include <atomic>
#include <iostream>
#include <string>
#include <sstream>

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

/* For tee request pad cleanup (important for long runs) */
static GstPad *tee_req_pad_udp = nullptr;
static GstPad *tee_req_pad_fps = nullptr;

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
 * BUS logging (mandatory)
 * ======================= */
static gboolean bus_cb(GstBus *, GstMessage *msg, gpointer) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::cerr << "[BUS][ERROR] " << (err ? err->message : "unknown") << "\n";
            if (dbg) std::cerr << "[BUS][DEBUG] " << dbg << "\n";
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            std::cerr << "[BUS][WARN] " << (err ? err->message : "unknown") << "\n";
            if (dbg) std::cerr << "[BUS][DEBUG] " << dbg << "\n";
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (pipeline && GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                GstState old_s, new_s, pending;
                gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
                std::cerr << "[BUS][STATE] pipeline "
                          << gst_element_state_get_name(old_s) << " -> "
                          << gst_element_state_get_name(new_s)
                          << " (pending " << gst_element_state_get_name(pending) << ")\n";
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

/* =======================
 * State helper
 * ======================= */
static void set_pipeline_state(GstState state, const char *name) {
    current_stage = name;
    std::cerr << "[STATE] -> " << name << std::endl;

    GstStateChangeReturn ret = gst_element_set_state(pipeline, state);

    // Block a bit to know if it really transitioned (helps with "no stream" cases)
    GstState cur = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(pipeline, &cur, &pending, 3 * GST_SECOND);

    std::cerr << "[STATE] set_state ret=" << ret
              << " cur=" << gst_element_state_get_name(cur)
              << " pending=" << gst_element_state_get_name(pending) << "\n";
}

/* =======================
 * State machine
 * ======================= */
enum class Phase { NULL_STATE, PLAYING, PAUSED, READY };

static Phase phase = Phase::NULL_STATE;
static int phase_mode = 3;

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

/* =======================
 * Tee → queue helper (returns requested pad)
 * ======================= */
static GstPad* link_tee_to_queue_and_keep_pad(GstElement *tee, GstElement *queue) {
    GstPad *tee_src = gst_element_request_pad_simple(tee, "src_%u");
    GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");

    if (!tee_src || !queue_sink) {
        if (tee_src) gst_object_unref(tee_src);
        if (queue_sink) gst_object_unref(queue_sink);
        return nullptr;
    }

    GstPadLinkReturn ret = gst_pad_link(tee_src, queue_sink);
    gst_object_unref(queue_sink);

    if (ret != GST_PAD_LINK_OK) {
        gst_object_unref(tee_src);
        return nullptr;
    }

    // Return tee_src (caller keeps it and later releases request pad)
    return tee_src;
}

/* =======================
 * Pipeline creation
 * ======================= */
static GstElement* create_pipeline(const std::string &host, int port) {
    tee_req_pad_udp = nullptr;
    tee_req_pad_fps = nullptr;

    GError *error = nullptr;

    // Build only the "main chain" up to tee using parse-launch
    // Then we manually add 2 tee branches:
    //  1) UDP RTP
    //  2) identity->fakesink for FPS counting
    std::ostringstream ss;
    ss <<
        "hailofrontendbinsrc config-file-path=/usr/bin/frontend_config_example.json name=preproc "
        "preproc.src_0 ! queue leaky=no max-size-buffers=1 ! fakesink sync=false "
        "preproc.src_1 ! queue leaky=no max-size-buffers=1 ! fakesink sync=false "
        "preproc.src_2 ! queue leaky=no max-size-buffers=1 ! fakesink sync=false "
        "preproc.src_3 ! queue leaky=no max-size-buffers=1 ! "
        "hailoencodebin config-file-path=/usr/bin/encoder_config_example.json ! "
        "h264parse config-interval=-1 ! "
        "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au ! "
        "tee name=fourk_enc_tee";

    GstElement *pipe = gst_parse_launch(ss.str().c_str(), &error);
    if (!pipe) {
        std::cerr << "[ERROR] gst_parse_launch failed\n";
        if (error) {
            std::cerr << error->message << "\n";
            g_error_free(error);
        }
        return nullptr;
    }
    if (error) g_error_free(error);

    // Add bus watch so errors aren't silent
    GstBus *bus = gst_element_get_bus(pipe);
    gst_bus_add_watch(bus, bus_cb, nullptr);
    gst_object_unref(bus);

    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipe), "fourk_enc_tee");
    if (!tee) {
        std::cerr << "[ERROR] tee 'fourk_enc_tee' not found\n";
        gst_object_unref(pipe);
        return nullptr;
    }

    // UDP branch
    GstElement *q_udp = gst_element_factory_make("queue", nullptr);
    GstElement *pay   = gst_element_factory_make("rtph264pay", nullptr);
    GstElement *udp   = gst_element_factory_make("udpsink", nullptr);

    // FPS branch
    GstElement *q_fps = gst_element_factory_make("queue", nullptr);
    GstElement *id    = gst_element_factory_make("identity", nullptr);
    GstElement *fsink = gst_element_factory_make("fakesink", nullptr);

    if (!q_udp || !pay || !udp || !q_fps || !id || !fsink) {
        std::cerr << "[ERROR] failed to create branch elements\n";
        gst_object_unref(tee);
        gst_object_unref(pipe);
        return nullptr;
    }

    g_object_set(pay, "pt", 96, nullptr);
    g_object_set(udp,
                 "host", host.c_str(),
                 "port", port,
                 "sync", FALSE,
                 "async", FALSE,
                 nullptr);

    g_object_set(id, "signal-handoffs", TRUE, nullptr);
    g_object_set(fsink, "sync", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipe), q_udp, pay, udp, q_fps, id, fsink, nullptr);

    if (!gst_element_link_many(q_udp, pay, udp, nullptr)) {
        std::cerr << "[ERROR] failed to link UDP branch\n";
    }
    if (!gst_element_link_many(q_fps, id, fsink, nullptr)) {
        std::cerr << "[ERROR] failed to link FPS branch\n";
    }

    tee_req_pad_udp = link_tee_to_queue_and_keep_pad(tee, q_udp);
    if (!tee_req_pad_udp) {
        std::cerr << "[ERROR] failed to link tee -> UDP queue\n";
    }

    tee_req_pad_fps = link_tee_to_queue_and_keep_pad(tee, q_fps);
    if (!tee_req_pad_fps) {
        std::cerr << "[ERROR] failed to link tee -> FPS queue\n";
    }

    g_signal_connect(id, "handoff", G_CALLBACK(on_handoff), nullptr);

    gst_object_unref(tee);
    return pipe;
}

/* =======================
 * State machine callback
 * ======================= */
static gboolean state_machine_cb(gpointer) {
    state_tick++;
    std::cerr << "[STATE_TICK] #" << state_tick
              << " (phase=" << phase_mode << ")\n";

    switch (phase) {
        case Phase::NULL_STATE:
            set_pipeline_state(GST_STATE_PLAYING, "PLAYING");
            phase = Phase::PLAYING;
            arm_next_state_timer(10);
            break;

        case Phase::PLAYING:
            set_pipeline_state(GST_STATE_PAUSED, "PAUSED");
            phase = Phase::PAUSED;
            arm_next_state_timer(1);
            break;

        case Phase::PAUSED:
            if (phase_mode == 4) {
                set_pipeline_state(GST_STATE_READY, "READY");
                phase = Phase::READY;
                arm_next_state_timer(1);
            } else {
                set_pipeline_state(GST_STATE_NULL, "NULL");

                // release tee request pads before unref (prevents long-run leaks)
                if (tee_req_pad_udp) { gst_object_unref(tee_req_pad_udp); tee_req_pad_udp = nullptr; }
                if (tee_req_pad_fps) { gst_object_unref(tee_req_pad_fps); tee_req_pad_fps = nullptr; }

                gst_object_unref(pipeline);
                pipeline = create_pipeline("10.0.0.2", 5000);
                phase = Phase::NULL_STATE;
                arm_next_state_timer(1);
            }
            break;

        case Phase::READY:
            set_pipeline_state(GST_STATE_NULL, "NULL");

            if (tee_req_pad_udp) { gst_object_unref(tee_req_pad_udp); tee_req_pad_udp = nullptr; }
            if (tee_req_pad_fps) { gst_object_unref(tee_req_pad_fps); tee_req_pad_fps = nullptr; }

            gst_object_unref(pipeline);
            pipeline = create_pipeline("10.0.0.2", 5000);
            phase = Phase::NULL_STATE;
            arm_next_state_timer(1);
            break;
    }

    return G_SOURCE_REMOVE;
}

static void print_help(const char *prog) {
    std::cout <<
        "Usage: " << prog << " [OPTIONS]\n\n"
        "Options:\n"
        "  -h, --help        Show this help message and exit\n"
        "   --phase 3        NULL → PLAYING → PAUSED → NULL (default)\n"
        "   --phase 4        NULL → PLAYING → PAUSED → READY → NULL\n\n"
        "Description:\n"
        "  GStreamer pipeline stress/aging test tool.\n"
        "  - Periodically switches pipeline state:\n"
        "      NULL -> PLAYING -> PAUSED -> NULL (recreate)\n"
        "  - Logs FPS every 5 seconds using identity handoff.\n"
        "  - Streams H.264 over UDP and measures internal frame flow.\n\n"
        "Signals:\n"
        "  SIGINT (Ctrl+C)    Graceful shutdown\n"
        << std::endl;
}

/* =======================
 * Main
 * ======================= */
int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        }

        if (arg == "--phase" && i + 1 < argc) {
            phase_mode = std::stoi(argv[i + 1]);
            i++;
        }
    }

    if (phase_mode != 3 && phase_mode != 4) {
        std::cerr << "[ERROR] --phase must be 3 or 4\n";
        return -1;
    }

    gst_init(&argc, &argv);
    std::signal(SIGINT, handle_sigint);

    pipeline = create_pipeline("10.0.0.2", 5000);
    if (!pipeline) return -1;

    loop = g_main_loop_new(nullptr, FALSE);

    g_timeout_add_seconds(5, fps_log_cb, nullptr);
    arm_next_state_timer(1);

    std::cerr << "[INFO] PLAYING=10s, PAUSED=1s, NULL=1s (recreate)\n";
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);

    if (tee_req_pad_udp) { gst_object_unref(tee_req_pad_udp); tee_req_pad_udp = nullptr; }
    if (tee_req_pad_fps) { gst_object_unref(tee_req_pad_fps); tee_req_pad_fps = nullptr; }

    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
