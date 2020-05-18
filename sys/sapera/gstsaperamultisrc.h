#ifndef _GST_SAPERA_MULTI_SRC_H_
#define _GST_SAPERA_MULTI_SRC_H_


#include <SapClassBasic.h>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

G_BEGIN_DECLS

#define GST_TYPE_SAPERA_MULTI_SRC   (gst_saperamultisrc_get_type())
#define GST_SAPERA_MULTI_SRC(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SAPERA_MULTI_SRC,GstSaperaMultiSrc))
#define GST_SAPERA_MULTI_SRC_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_SAPERA_MULTI_SRC,GstSaperaMultiSrcClass))
#define GST_IS_SAPERA_MULTI_SRC(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SAPERA_MULTI_SRC))
#define GST_IS_SAPERA_MULTI_SRC_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SAPERA_MULTI_SRC))

typedef struct _GstSaperaMultiSrc GstSaperaMultiSrc;
typedef struct _GstSaperaMultiSrcClass GstSaperaMultiSrcClass;

class SapGstMultiProcessing;

struct _GstSaperaMultiSrc
{
  GstPushSrc base_saperamultisrc;

  guint last_buffer_number;
  gint dropped_frame_count;
  gboolean acq_started;

  /* Sapera objects */
  SapAcquisition *sap_acq[2];
  SapBufferRoi   *sap_roi_buffers[2];
  SapBuffer      *sap_buffers;
  SapBayer       *sap_bayer;
  SapTransfer    *sap_xfer[2];
  SapGstMultiProcessing*sap_pro;

  /* properties */
  gchar *format_file;
  guint num_capture_buffers;
  gint server_index;
  gint resource_index;
  gint channel_extract;

  GstBuffer *buffer;

  GstCaps *caps;
  gint width;
  gint height;
  gint gst_stride;

  GMutex buffer_mutex;
  GCond buffer_cond;
};

struct _GstSaperaMultiSrcClass
{
  GstPushSrcClass base_saperamultisrc_class;
};

GType gst_saperamultisrc_get_type (void);

G_END_DECLS


#endif
