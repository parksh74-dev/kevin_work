gst_dep      = dependency('gstreamer-1.0', required: true)
gstapp_dep   = dependency('gstreamer-app-1.0', required: true)
glib_dep     = dependency('glib-2.0', required: true)

executable(
  'appsrc_sender',
  'main.cpp',
  dependencies: [gst_dep, gstapp_dep, glib_dep],
  install: false
)

