#define PACKAGE "rawcapturebypass"
#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <stdio.h>
#include <unistd.h> // for access() and unlink()

#define GST_TYPE_RAWCAPTUREBYPASS   (gst_rawcapture_bypass_get_type())
G_DECLARE_FINAL_TYPE(GstRawCaptureBypass, gst_rawcapture_bypass, GST, RAWCAPTUREBYPASS, GstBaseTransform)

struct _GstRawCaptureBypass {
  GstBaseTransform parent;
};

G_DEFINE_TYPE(GstRawCaptureBypass, gst_rawcapture_bypass, GST_TYPE_BASE_TRANSFORM)

static GstFlowReturn
gst_rawcapture_bypass_transform_ip(GstBaseTransform *trans, GstBuffer *buf)
{
  // Check for the capture flag file
  if (access("/tmp/capture_flag", F_OK) == 0) {
    FILE *outfile = fopen("kevin_nv12.raw", "wb");
    if (outfile) {
      GstMapInfo info;
      if (gst_buffer_map(buf, &info, GST_MAP_READ)) {
        fwrite(info.data, 1, info.size, outfile);
        gst_buffer_unmap(buf, &info);
        g_print("Captured NV12 frame to kevin_nv12.raw\n");
      }
      fclose(outfile);
    } else {
      g_warning("Could not open kevin_nv12.raw for writing");
    }
    // Remove the flag file after capturing
    unlink("/tmp/capture_flag");
  }

  // Pass buffer through (in-place transform)
  return GST_FLOW_OK;
}

static void
gst_rawcapture_bypass_class_init(GstRawCaptureBypassClass *klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

  // Only accept video/x-raw with NV12 format on src and sink
  GstCaps *caps = gst_caps_new_simple(
      "video/x-raw",
      "format", G_TYPE_STRING, "NV12",
      NULL
  );
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new(
          "sink",
          GST_PAD_SINK,
          GST_PAD_ALWAYS,
          caps
      )
  );
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new(
          "src",
          GST_PAD_SRC,
          GST_PAD_ALWAYS,
          caps
      )
  );
  gst_caps_unref(caps);

  gst_element_class_set_metadata(
    element_class,
    "NV12 Raw Capture Bypass Filter",
    "Filter/Effect/Bypass",
    "Bypasses NV12 buffers and saves a raw file when /tmp/capture_flag is created",
    "Kevin P <your.email@example.com>"
  );

  GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
  base_transform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_rawcapture_bypass_transform_ip);
}

static void
gst_rawcapture_bypass_init(GstRawCaptureBypass *self) {}

static gboolean
plugin_init(GstPlugin *plugin)
{
  return gst_element_register(plugin, "rawcapturebypass", GST_RANK_NONE, GST_TYPE_RAWCAPTUREBYPASS);
}

GST_PLUGIN_DEFINE(
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  rawcapturebypass,
  "Bypass NV12 filter that saves frame when /tmp/capture_flag exists",
  plugin_init,
  "1.0",
  "LGPL",
  "GStreamer",
  "https://gstreamer.freedesktop.org/"
)
