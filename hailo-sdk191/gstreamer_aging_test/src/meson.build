gst_dep = dependency('gstreamer-1.0', required : true)
glib_dep = dependency('glib-2.0', required : true)

executable(
  'gst_cycle',
  'gst_cycle.cpp',
  dependencies : [
    gst_dep,
    glib_dep,
  ],
  install : true
)
