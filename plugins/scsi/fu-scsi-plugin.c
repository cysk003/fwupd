/*
 * Copyright 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-scsi-device.h"
#include "fu-scsi-plugin.h"

struct _FuScsiPlugin {
	FuPlugin parent_instance;
};

G_DEFINE_TYPE(FuScsiPlugin, fu_scsi_plugin, FU_TYPE_PLUGIN)

static void
fu_scsi_plugin_init(FuScsiPlugin *self)
{
}

static void
fu_scsi_plugin_constructed(GObject *obj)
{
	FuPlugin *plugin = FU_PLUGIN(obj);
	FuContext *ctx = fu_plugin_get_context(plugin);

	fu_context_add_quirk_key(ctx, "ScsiWriteBufferSize");
	fu_plugin_add_device_udev_subsystem(plugin, "block:disk");
	fu_plugin_add_device_gtype(plugin, FU_TYPE_SCSI_DEVICE);
}

static void
fu_scsi_plugin_class_init(FuScsiPluginClass *klass)
{
	FuPluginClass *plugin_class = FU_PLUGIN_CLASS(klass);
	plugin_class->constructed = fu_scsi_plugin_constructed;
}
