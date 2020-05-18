#include "gstsaperamultisrc.h"

/**
 * SECTION:element-gstsaperamultisrc
 *
 * The saperamultisrc element is a source for Teledyne DALSA Sapera framegrabbers.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v saperamultisrc ! ffmpegcolorspace ! autovideosink
 * ]|
 * Shows video from the default DALSA framegrabber
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/video/video.h>

#include "gstsaperamultisrc.h"


gboolean gst_saperamultisrc_create_objects (GstSaperaMultiSrc * src);
gboolean gst_saperamultisrc_destroy_objects (GstSaperaMultiSrc * src);

class SapGstMultiProcessing:public SapProcessing
{
public:
  SapGstMultiProcessing (SapBuffer * pBuffers, SapProCallback pCallback,
      void *pContext)
  : SapProcessing (pBuffers, pCallback, pContext)
  {
    src = (GstSaperaMultiSrc *) pContext;
  }

  virtual ~ SapGstMultiProcessing ()
  {
    if (m_bInitOK)
      Destroy ();
  }

protected:
  virtual BOOL Run () {
    // TODO: handle bayer
    //if (src->sap_bayer->IsEnabled () && src->sap_bayer->IsSoftware ()) {
    //    src->sap_bayer->Convert (GetIndex());
    //}

    push_buffer ();

    return TRUE;
  }

  gboolean push_buffer ()
  {
    void *pData;
    GstMapInfo minfo;

    // TODO: check for failure
    src->sap_buffers->GetAddress (&pData);
    int pitch = src->sap_buffers->GetPitch ();
    int height = src->sap_buffers->GetHeight ();
    gssize size = pitch * height;

    GstBuffer *buf;
    /* create a new buffer assign to it the clock time as timestamp */
    buf = gst_buffer_new_and_alloc (size);

    gst_buffer_set_size (buf, size);

    GstClock *clock = gst_element_get_clock (GST_ELEMENT (src));
    GST_BUFFER_TIMESTAMP (buf) =
        GST_CLOCK_DIFF (gst_element_get_base_time (GST_ELEMENT (src)),
        gst_clock_get_time (clock));
    gst_object_unref (clock);

    // TODO: define duration?
    //GST_BUFFER_DURATION (buf) = duration;

    if (!gst_buffer_map (buf, &minfo, GST_MAP_WRITE)) {
      gst_buffer_unref (buf);
      GST_ERROR_OBJECT (src, "Failed to map buffer");
      return FALSE;
    }
    // TODO: optimize this
    if (src->channel_extract == 0) {
      if (pitch == src->gst_stride) {
        memcpy (minfo.data, pData, size);
      } else {
        for (int line = 0; line < src->height; line++) {
          memcpy (minfo.data + (line * src->gst_stride),
              (guint8 *) pData + (line * pitch), pitch);
        }
      }
    } else {
      guint32 mask = 0x0, shift = 0;
      if (src->channel_extract == 1) {
        mask = 0x3ff00000;
        shift = 20;
      } else if (src->channel_extract == 2) {
        mask = 0xffc00;
        shift = 10;
      } else if (src->channel_extract == 3) {
        mask = 0x3ff;
        shift = 0;
      } else
        g_assert_not_reached ();

      guint32 *packed = (guint32 *) pData;
      guint16 *dst = (guint16 *) minfo.data;
      for (int r = 0; r < src->height; ++r) {
        for (int c = 0; c < src->width; ++c) {
          *dst = (*packed & mask) >> shift;
          ++dst;
          ++packed;
        }
      }
    }

    gst_buffer_unmap (buf, &minfo);

    GST_DEBUG ("push_buffer => pts %" GST_TIME_FORMAT,
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

    g_mutex_lock (&src->buffer_mutex);
    if (src->buffer != NULL)
      gst_buffer_unref (src->buffer);
    src->buffer = buf;
    g_cond_signal (&src->buffer_cond);
    g_mutex_unlock (&src->buffer_mutex);

    return TRUE;
  }

protected:
  GstSaperaMultiSrc * src;
};

void
gst_saperamultisrc_xfer_callback (SapXferCallbackInfo * pInfo)
{
  GstSaperaMultiSrc *src = (GstSaperaMultiSrc *) pInfo->GetContext ();

  if (pInfo->IsTrash ()) {
    /* TODO: update dropped buffer count */
  } else {
    /* Process current buffer */
    src->sap_pro->Execute ();
  }
}

void
gst_saperamultisrc_pro_callback (SapProCallbackInfo * pInfo)
{
  /* GstSaperaMultiSrc *src = (GstSaperaMultiSrc *) pInfo->GetContext (); */

  /* TODO: handle buffer */
}

gboolean
gst_saperamultisrc_init_objects (GstSaperaMultiSrc * src)
{
  char name[128];
  int server_count, resource_count;

  server_count = SapManager::GetServerCount ();
  GST_DEBUG_OBJECT (src, "There are %d servers available", server_count);

  if (src->server_index > server_count ||
      !SapManager::GetServerName (src->server_index, name)) {
    GST_ERROR_OBJECT (src, "Invalid server index %d", src->server_index);
    return FALSE;
  }

  GST_DEBUG_OBJECT (src, "Trying to open server index %d ('%s')",
      src->server_index, name);

  resource_count = SapManager::GetResourceCount (src->server_index,
      SapManager::ResourceAcq);
  GST_DEBUG_OBJECT (src, "Resource count: %d", resource_count);

  if (src->resource_index > resource_count ||
      !SapManager::GetResourceName (src->server_index, SapManager::ResourceAcq,
          src->resource_index, name, 128)) {
    GST_ERROR_OBJECT (src, "Invalid resource index %d", src->resource_index);
    return FALSE;
  }
  GST_DEBUG_OBJECT (src, "Trying to open resource index %d ('%s')",
      src->resource_index, name);

  GST_DEBUG_OBJECT (src, "Using config file '%s'", src->format_file);

  SapLocation loc (src->server_index, src->resource_index);
  SapLocation loc2 (src->server_index + 1, src->resource_index);
  src->sap_acq[0] = new SapAcquisition (loc, src->format_file);
  src->sap_acq[1] = new SapAcquisition (loc2, src->format_file);

  /* TODO: allow configuring buffer count? */
  src->sap_buffers = new SapBufferWithTrash ();
  src->sap_roi_buffers[0] = new SapBufferRoi (src->sap_buffers);
  src->sap_roi_buffers[1] = new SapBufferRoi (src->sap_buffers);

  src->sap_xfer[0] =
      new SapAcqToBuf (src->sap_acq[0], src->sap_roi_buffers[0],
      gst_saperamultisrc_xfer_callback, src);
  src->sap_xfer[1] =
      new SapAcqToBuf (src->sap_acq[1], src->sap_roi_buffers[1],
      gst_saperamultisrc_xfer_callback, src);

  // TODO: handle bayer
  //src->sap_bayer = new SapBayer(m_Acq, m_Buffers);
  src->sap_pro =
      new SapGstMultiProcessing (src->sap_buffers, gst_saperamultisrc_pro_callback, src);

  return TRUE;
}

static gboolean
is_scatter_gatter_supported(SapLocation loc1, SapLocation loc2)
{
  return TRUE
    && SapBuffer::IsBufferTypeSupported(loc1, SapBuffer::TypeScatterGather)
    && SapBuffer::IsBufferTypeSupported(loc2, SapBuffer::TypeScatterGather)
    ;
}


static gboolean
is_scatter_gatter_physical_supported(SapLocation loc1, SapLocation loc2)
{
  return TRUE
    && !SapBuffer::IsBufferTypeSupported(loc1, SapBuffer::TypeScatterGather)
    && !SapBuffer::IsBufferTypeSupported(loc2, SapBuffer::TypeScatterGather)
    && SapBuffer::IsBufferTypeSupported(loc1, SapBuffer::TypeScatterGatherPhysical)
    && SapBuffer::IsBufferTypeSupported(loc2, SapBuffer::TypeScatterGatherPhysical)
    ;
}

gboolean
gst_saperamultisrc_create_objects (GstSaperaMultiSrc * src)
{
  //UINT32 video_type = 0;

  /* Create acquisition object */
  if (src->sap_acq[0] && !*src->sap_acq[0]) {
    if (!src->sap_acq[0]->Create ()) {
      GST_ERROR_OBJECT (src, "Failed to create SapAcquisition 1");
      gst_saperamultisrc_destroy_objects (src);
      return FALSE;
    }
  }

  if (src->sap_acq[1] && !*src->sap_acq[1]) {
    if (!src->sap_acq[1]->Create ()) {
      GST_ERROR_OBJECT (src, "Failed to create SapAcquisition 2");
      gst_saperamultisrc_destroy_objects (src);
      return FALSE;
    }
  }

  int width = src->sap_acq[0]->GetXferParams().GetWidth();
  int height = src->sap_acq[0]->GetXferParams().GetHeight();
  SapFormat format = src->sap_acq[0]->GetXferParams().GetFormat();

  if (is_scatter_gatter_supported(src->sap_acq[0]->GetLocation(), src->sap_acq[1]->GetLocation())) {
    src->sap_buffers->SetParameters(1, 2*width, height, format, SapBuffer::TypeScatterGather);
  }
  else if (is_scatter_gatter_physical_supported(src->sap_acq[0]->GetLocation(), src->sap_acq[1]->GetLocation())) {
    src->sap_buffers->SetParameters(1, 2*width, height, format, SapBuffer::TypeScatterGatherPhysical);
  }
  else {
    GST_ERROR_OBJECT (src, "Scatter Gatter Not Supported");
    gst_saperamultisrc_destroy_objects (src);
    return FALSE;
  }

  int pixel_depth = src->sap_acq[0]->GetXferParams().GetPixelDepth();
  src->sap_buffers->SetPixelDepth(pixel_depth);

  // Create buffer objects
  if (src->sap_buffers && !*src->sap_buffers) {
    if (!src->sap_buffers->Create ()) {
      GST_ERROR_OBJECT (src, "Failed to create SapBuffer");
      gst_saperamultisrc_destroy_objects (src);
      return FALSE;
    }
    // Clear all buffers
    src->sap_buffers->Clear ();
  }

  src->sap_roi_buffers[0]->SetRoi(0, 0, width, height);
  if (!src->sap_roi_buffers[0]->Create()) {
    GST_ERROR_OBJECT (src, "Failed to create SapBuffer Roi 1");
    gst_saperamultisrc_destroy_objects (src);
    return FALSE;
  }
  src->sap_roi_buffers[1]->SetRoi(width, 0, width, height);
  if (!src->sap_roi_buffers[1]->Create()) {
    GST_ERROR_OBJECT (src, "Failed to create SapBuffer Roi 2");
    gst_saperamultisrc_destroy_objects (src);
    return FALSE;
  }

  /* Create transfer object */
  if (src->sap_xfer[0] && !*src->sap_xfer[0]) {
    if (!src->sap_xfer[0]->Create ()) {
        GST_ERROR_OBJECT (src, "Failed to create SapTransfer 1");
      gst_saperamultisrc_destroy_objects (src);
      return FALSE;
    }
  }

  if (src->sap_xfer[1] && !*src->sap_xfer[1]) {
    if (!src->sap_xfer[1]->Create ()) {
        GST_ERROR_OBJECT (src, "Failed to create SapTransfer 2");
      gst_saperamultisrc_destroy_objects (src);
      return FALSE;
    }
  }

  /* Create processing object */
  if (src->sap_pro && !*src->sap_pro) {
    if (!src->sap_pro->Create ()) {
        GST_ERROR_OBJECT (src, "Failed to create SapProcessing");
      gst_saperamultisrc_destroy_objects (src);
      return FALSE;
    }

    src->sap_pro->SetAutoEmpty (TRUE);
  }

  return TRUE;
}

gboolean
gst_saperamultisrc_destroy_objects (GstSaperaMultiSrc * src)
{
  if (src->sap_xfer[0] && *src->sap_xfer[0])
    src->sap_xfer[0]->Destroy ();

  if (src->sap_xfer[1] && *src->sap_xfer[1])
    src->sap_xfer[1]->Destroy ();

  if (src->sap_pro && *src->sap_pro)
    src->sap_pro->Destroy ();

  // TODO: handle bayer
  //if (src->sap_bayer && *src->sap_bayer) src->sap_bayer->Destroy ();

  if (src->sap_roi_buffers[0] && *src->sap_roi_buffers[0])
    src->sap_roi_buffers[0]->Destroy ();

  if (src->sap_roi_buffers[1] && *src->sap_roi_buffers[1])
    src->sap_roi_buffers[1]->Destroy ();

  if (src->sap_buffers && *src->sap_buffers)
    src->sap_buffers->Destroy ();

  if (src->sap_acq[0] && *src->sap_acq[0])
    src->sap_acq[0]->Destroy ();

  if (src->sap_acq[1] && *src->sap_acq[1])
    src->sap_acq[1]->Destroy ();

  return TRUE;
}

/* prototypes */
static void gst_saperamultisrc_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_saperamultisrc_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_saperamultisrc_dispose (GObject * object);
static void gst_saperamultisrc_finalize (GObject * object);

static gboolean gst_saperamultisrc_start (GstBaseSrc * src);
static gboolean gst_saperamultisrc_stop (GstBaseSrc * src);
static GstCaps *gst_saperamultisrc_get_caps (GstBaseSrc * src, GstCaps * filter);
static gboolean gst_saperamultisrc_set_caps (GstBaseSrc * src, GstCaps * caps);

static GstFlowReturn gst_saperamultisrc_create (GstPushSrc * src, GstBuffer ** buf);

static GstCaps *gst_saperamultisrc_create_caps (GstSaperaMultiSrc * src);

enum
{
  PROP_0,
  PROP_FORMAT_FILE,
  PROP_NUM_CAPTURE_BUFFERS,
  PROP_SERVER_INDEX,
  PROP_RESOURCE_INDEX,
  PROP_CHANNEL_EXTRACT
};

#define DEFAULT_PROP_FORMAT_FILE ""
#define DEFAULT_PROP_NUM_CAPTURE_BUFFERS 2
#define DEFAULT_PROP_SERVER_INDEX 1
#define DEFAULT_PROP_RESOURCE_INDEX 0
#define DEFAULT_PROP_CHANNEL_EXTRACT 0

/* pad templates */

static GstStaticPadTemplate gst_saperamultisrc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
        ("{ GRAY8, GRAY16_LE, GRAY16_BE, BGR, BGRA }"))
    );

/* class initialization */

G_DEFINE_TYPE (GstSaperaMultiSrc, gst_saperamultisrc, GST_TYPE_PUSH_SRC);

static void
gst_saperamultisrc_class_init (GstSaperaMultiSrcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *gstpushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_saperamultisrc_set_property;
  gobject_class->get_property = gst_saperamultisrc_get_property;
  gobject_class->dispose = gst_saperamultisrc_dispose;
  gobject_class->finalize = gst_saperamultisrc_finalize;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_saperamultisrc_src_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "Teledyne DALSA Sapera Video Source", "Source/Video",
      "Teledyne DALSA Sapera framegrabber video source",
      "Joshua M. Doe <oss@nvl.army.mil>");

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_saperamultisrc_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_saperamultisrc_stop);
  gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_saperamultisrc_get_caps);
  gstbasesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_saperamultisrc_set_caps);

  gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_saperamultisrc_create);

  /* Install GObject properties */
  g_object_class_install_property (gobject_class, PROP_FORMAT_FILE,
      g_param_spec_string ("config-file", "Config file",
          "Camera configuration filepath",
          DEFAULT_PROP_FORMAT_FILE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
              GST_PARAM_MUTABLE_READY)));
  g_object_class_install_property (gobject_class, PROP_NUM_CAPTURE_BUFFERS,
      g_param_spec_uint ("num-capture-buffers", "Number of capture buffers",
          "Number of capture buffers", 1, G_MAXUINT,
          DEFAULT_PROP_NUM_CAPTURE_BUFFERS,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_SERVER_INDEX,
      g_param_spec_int ("server-index", "Server index",
          "Server (frame grabber card) index", 0, G_MAXINT,
          DEFAULT_PROP_SERVER_INDEX,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_RESOURCE_INDEX,
      g_param_spec_int ("resource-index", "Resource index",
          "Resource index, such as different ports or configurations", 0,
          G_MAXINT, DEFAULT_PROP_RESOURCE_INDEX,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_CHANNEL_EXTRACT,
      g_param_spec_int ("color-channel", "Color channel", "Color channel", 0, 3,
          DEFAULT_PROP_CHANNEL_EXTRACT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_saperamultisrc_reset (GstSaperaMultiSrc * src)
{
  src->dropped_frame_count = 0;
  src->last_buffer_number = 0;
  src->acq_started = FALSE;

  if (src->caps) {
    gst_caps_unref (src->caps);
    src->caps = NULL;
  }
  if (src->buffer) {
    gst_buffer_unref (src->buffer);
    src->buffer = NULL;
  }

  gst_saperamultisrc_destroy_objects (src);

  if (src->sap_acq[0]) {
    delete src->sap_acq[0];
    src->sap_acq[0] = NULL;
  }
  if (src->sap_acq[1]) {
    delete src->sap_acq[1];
    src->sap_acq[1] = NULL;
  }
  if (src->sap_roi_buffers[0]) {
    delete src->sap_roi_buffers[0];
    src->sap_roi_buffers[0] = NULL;
  }
  if (src->sap_roi_buffers[1]) {
    delete src->sap_roi_buffers[1];
    src->sap_roi_buffers[1] = NULL;
  }
  if (src->sap_buffers) {
    delete src->sap_buffers;
    src->sap_buffers = NULL;
  }
  if (src->sap_bayer) {
    delete src->sap_bayer;
    src->sap_bayer = NULL;
  }
  if (src->sap_xfer[0]) {
    delete src->sap_xfer[0];
    src->sap_xfer[0] = NULL;
  }
  if (src->sap_xfer[1]) {
    delete src->sap_xfer[1];
    src->sap_xfer[1] = NULL;
  }
  if (src->sap_pro) {
    delete src->sap_pro;
    src->sap_pro = NULL;
  }
}

static void
gst_saperamultisrc_init (GstSaperaMultiSrc * src)
{
  /* set source as live (no preroll) */
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);

  /* override default of BYTES to operate in time mode */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  /* initialize member variables */
  src->format_file = g_strdup (DEFAULT_PROP_FORMAT_FILE);
  src->num_capture_buffers = DEFAULT_PROP_NUM_CAPTURE_BUFFERS;

  g_mutex_init (&src->buffer_mutex);
  g_cond_init (&src->buffer_cond);

  src->caps = NULL;
  src->buffer = NULL;

  src->sap_acq[0] = NULL;
  src->sap_acq[1] = NULL;
  src->sap_roi_buffers[0] = NULL;
  src->sap_roi_buffers[1] = NULL;
  src->sap_buffers = NULL;
  src->sap_bayer = NULL;
  src->sap_xfer[0] = NULL;
  src->sap_xfer[1] = NULL;
  src->sap_pro = NULL;

  gst_saperamultisrc_reset (src);
}

void
gst_saperamultisrc_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSaperaMultiSrc *src;

  src = GST_SAPERA_MULTI_SRC (object);

  switch (property_id) {
    case PROP_FORMAT_FILE:
      g_free (src->format_file);
      src->format_file = g_strdup (g_value_get_string (value));
      break;
    case PROP_NUM_CAPTURE_BUFFERS:
      if (src->acq_started) {
        GST_ELEMENT_WARNING (src, RESOURCE, SETTINGS,
            ("Number of capture buffers cannot be changed after acquisition has started."),
            (NULL));
      } else {
        src->num_capture_buffers = g_value_get_uint (value);
      }
      break;
    case PROP_SERVER_INDEX:
      src->server_index = g_value_get_int (value);
      break;
    case PROP_RESOURCE_INDEX:
      src->resource_index = g_value_get_int (value);
      break;
    case PROP_CHANNEL_EXTRACT:
      src->channel_extract = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_saperamultisrc_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSaperaMultiSrc *src;

  g_return_if_fail (GST_IS_SAPERA_MULTI_SRC (object));
  src = GST_SAPERA_MULTI_SRC (object);

  switch (property_id) {
    case PROP_FORMAT_FILE:
      g_value_set_string (value, src->format_file);
      break;
    case PROP_NUM_CAPTURE_BUFFERS:
      g_value_set_uint (value, src->num_capture_buffers);
      break;
    case PROP_SERVER_INDEX:
      g_value_set_int (value, src->server_index);
      break;
    case PROP_RESOURCE_INDEX:
      g_value_set_int (value, src->resource_index);
      break;
    case PROP_CHANNEL_EXTRACT:
      g_value_set_int (value, src->channel_extract);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_saperamultisrc_dispose (GObject * object)
{
  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_saperamultisrc_parent_class)->dispose (object);
}

void
gst_saperamultisrc_finalize (GObject * object)
{
  GstSaperaMultiSrc *src;

  g_return_if_fail (GST_IS_SAPERA_MULTI_SRC (object));
  src = GST_SAPERA_MULTI_SRC (object);

  /* clean up object here */
  g_free (src->format_file);

  if (src->caps) {
    gst_caps_unref (src->caps);
    src->caps = NULL;
  }

  if (src->buffer) {
    gst_buffer_unref (src->buffer);
    src->buffer = NULL;
  }

  G_OBJECT_CLASS (gst_saperamultisrc_parent_class)->finalize (object);
}

static gboolean
gst_saperamultisrc_start (GstBaseSrc * bsrc)
{
  GstSaperaMultiSrc *src = GST_SAPERA_MULTI_SRC (bsrc);
  GstVideoInfo vinfo;
  SapFormat sap_format;
  GstVideoFormat gst_format;

  GST_DEBUG_OBJECT (src, "start");

  if (!strlen (src->format_file)) {
    GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
        ("Configuration file has not been specified"), (NULL));
    return FALSE;
  }

  if (!g_file_test (src->format_file, G_FILE_TEST_EXISTS)) {
    GST_ELEMENT_ERROR (src, RESOURCE, NOT_FOUND,
        ("Configuration file does not exist: %s", src->format_file), (NULL));
    return FALSE;
  }

  GST_DEBUG_OBJECT (src, "About to initialize and create Sapera objects");
  if (!gst_saperamultisrc_init_objects (src) || !gst_saperamultisrc_create_objects (src)) {
    GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
        ("Failed to create Sapera objects"), (NULL));
    return FALSE;
  }

  GST_DEBUG_OBJECT (src, "Creating caps from Sapera buffer format");
  sap_format = src->sap_buffers->GetFormat ();
  switch (sap_format) {
    case SapFormatMono8:
      gst_format = GST_VIDEO_FORMAT_GRAY8;
      break;
    case SapFormatMono16:
      gst_format = GST_VIDEO_FORMAT_GRAY16_LE;
      break;
    case SapFormatRGB888:
      gst_format = GST_VIDEO_FORMAT_BGR;
      break;
    case SapFormatRGB8888:
      gst_format = GST_VIDEO_FORMAT_BGRA;
      break;
    case SapFormatRGB101010:
      gst_format = GST_VIDEO_FORMAT_GRAY16_LE;
      break;
    default:
      gst_format = GST_VIDEO_FORMAT_UNKNOWN;
  }

  if (gst_format == GST_VIDEO_FORMAT_UNKNOWN) {
    char format_name[17];
    SapManager::GetStringFromFormat (sap_format, format_name);
    GST_ELEMENT_ERROR (src, RESOURCE, FAILED,
        ("Unsupported format: %s", format_name), (NULL));
    return FALSE;
  }

  gst_video_info_init (&vinfo);
  gst_video_info_set_format (&vinfo, gst_format, src->sap_buffers->GetWidth (), src->sap_buffers->GetHeight ());
  src->caps = gst_video_info_to_caps (&vinfo);

  src->width = vinfo.width;
  src->height = vinfo.height;
  src->gst_stride = GST_VIDEO_INFO_COMP_STRIDE (&vinfo, 0);

  if (!src->sap_xfer[0]->Grab ()) {
    GST_ELEMENT_ERROR (src, RESOURCE, FAILED, ("Failed to start grab transfer 1"), (NULL));
    return FALSE;
  }
  if (!src->sap_xfer[1]->Grab ()) {
    GST_ELEMENT_ERROR (src, RESOURCE, FAILED, ("Failed to start grab transfer 2"), (NULL));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_saperamultisrc_stop (GstBaseSrc * bsrc)
{
  GstSaperaMultiSrc *src = GST_SAPERA_MULTI_SRC (bsrc);

  GST_DEBUG_OBJECT (src, "stop");

  if (!src->sap_xfer[0]->Freeze ()) {
    GST_ERROR_OBJECT (src, "Failed to stop camera acquisition 1");
    return FALSE;
  }

  if (!src->sap_xfer[1]->Freeze ()) {
    GST_ERROR_OBJECT (src, "Failed to stop camera acquisition 2");
    return FALSE;
  }

  if (!src->sap_xfer[0]->Wait (250)) {
    GST_ERROR_OBJECT (src, "Acquisition 1 failed to stop camera, aborting");
    src->sap_xfer[0]->Abort ();
    return FALSE;
  }

  if (!src->sap_xfer[1]->Wait (250)) {
    GST_ERROR_OBJECT (src, "Acquisition 2 failed to stop camera, aborting");
    src->sap_xfer[1]->Abort ();
    return FALSE;
  }

  gst_saperamultisrc_reset (src);

  return TRUE;
}

static GstCaps *
gst_saperamultisrc_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
  GstSaperaMultiSrc *src = GST_SAPERA_MULTI_SRC (bsrc);
  GstCaps *caps;

  if (src->sap_acq[0] && *src->sap_acq[0]) {
    caps = gst_caps_copy (src->caps);
  } else {
    caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (src));
  }

  GST_DEBUG_OBJECT (src, "The caps before filtering are %" GST_PTR_FORMAT,
      caps);

  if (filter && caps) {
    GstCaps *tmp = gst_caps_intersect (caps, filter);
    gst_caps_unref (caps);
    caps = tmp;
  }

  GST_DEBUG_OBJECT (src, "The caps after filtering are %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
gst_saperamultisrc_set_caps (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstSaperaMultiSrc *src = GST_SAPERA_MULTI_SRC (bsrc);
  GstVideoInfo vinfo;

  GST_DEBUG_OBJECT (src, "The caps being set are %" GST_PTR_FORMAT, caps);

  gst_video_info_from_caps (&vinfo, caps);

  if (GST_VIDEO_INFO_FORMAT (&vinfo) != GST_VIDEO_FORMAT_UNKNOWN) {
    src->gst_stride = GST_VIDEO_INFO_COMP_STRIDE (&vinfo, 0);
  } else {
    goto unsupported_caps;
  }

  return TRUE;

unsupported_caps:
  GST_ERROR_OBJECT (src, "Unsupported caps: %" GST_PTR_FORMAT, caps);
  return FALSE;
}

static GstFlowReturn
gst_saperamultisrc_create (GstPushSrc * psrc, GstBuffer ** buf)
{
  GstSaperaMultiSrc *src = GST_SAPERA_MULTI_SRC (psrc);

  g_mutex_lock (&src->buffer_mutex);
  while (src->buffer == NULL)
    g_cond_wait (&src->buffer_cond, &src->buffer_mutex);
  *buf = src->buffer;
  src->buffer = NULL;
  g_mutex_unlock (&src->buffer_mutex);

  GST_DEBUG ("saperamultisrc_create => pts %" GST_TIME_FORMAT " duration %"
      GST_TIME_FORMAT, GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (*buf)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (*buf)));

  return GST_FLOW_OK;
}
