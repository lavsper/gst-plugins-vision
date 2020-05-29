/* GStreamer
 * Copyright (C) 2011 FIXME <fixme@example.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsaperasrc.h"
#include "gstsaperamultisrc.h"


static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "sapera", 0,
      "debug category for sapera");

  GST_DEBUG ("plugin_init");

  GST_CAT_INFO (GST_CAT_DEFAULT, "registering saperasrc element");
  if (!gst_element_register (plugin, "saperasrc", GST_RANK_NONE,
        gst_saperasrc_get_type ())) {
    return FALSE;
  }

  GST_CAT_INFO (GST_CAT_DEFAULT, "registering saperamultisrc element");
  if (!gst_element_register (plugin, "saperamultisrc", GST_RANK_NONE,
      gst_saperamultisrc_get_type ())) {
    return FALSE;
  }

  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    sapera,
    "Teledyne DALSA Sapera frame grabber",
    plugin_init, GST_PACKAGE_VERSION, GST_PACKAGE_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
