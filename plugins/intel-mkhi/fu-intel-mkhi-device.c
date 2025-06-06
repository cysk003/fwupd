/*
 * Copyright 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-intel-mkhi-device.h"

struct _FuIntelMkhiDevice {
	FuHeciDevice parent_instance;
};

G_DEFINE_TYPE(FuIntelMkhiDevice, fu_intel_mkhi_device, FU_TYPE_HECI_DEVICE)

#define FU_INTEL_MKHI_DEVICE_FLAG_LEAKED_KM "leaked-km"

static gboolean
fu_intel_mkhi_device_add_checksum_for_filename(FuIntelMkhiDevice *self,
					       const gchar *filename,
					       GError **error)
{
	g_autofree gchar *checksum = NULL;
	g_autoptr(GByteArray) buf = NULL;

	/* read from the MFS */
	buf = fu_heci_device_read_file(FU_HECI_DEVICE(self), filename, error);
	if (buf == NULL)
		return FALSE;

	/* convert into checksum, but only if non-zero and set */
	checksum = fu_byte_array_to_string(buf);
	if (g_str_has_prefix(checksum, "0000000000000000") ||
	    g_str_has_prefix(checksum, "ffffffffffffffff")) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "checksum %s was invalid",
			    checksum);
		return FALSE;
	}
	fu_device_add_checksum(FU_DEVICE(self), checksum);

	/* success */
	return TRUE;
}

static gboolean
fu_intel_mkhi_device_setup(FuDevice *device, GError **error)
{
	FuIntelMkhiDevice *self = FU_INTEL_MKHI_DEVICE(device);

	/* connect */
	if (!fu_mei_device_connect(FU_MEI_DEVICE(device), FU_HECI_DEVICE_UUID_MKHI, 0, error)) {
		g_prefix_error(error, "failed to connect: ");
		return FALSE;
	}

	/* this is the legacy way to get the hash, which is removed in newer ME versions due to
	 * possible path traversal attacks */
	if (!fu_intel_mkhi_device_add_checksum_for_filename(self, "/fpf/OemCred", error))
		return FALSE;

	/* no point even adding */
	if (fu_device_get_checksums(device)->len == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no OEM public keys found");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static void
fu_intel_mkhi_device_version_notify_cb(FuDevice *device, GParamSpec *pspec, gpointer user_data)
{
	if (fu_device_has_private_flag(device, FU_INTEL_MKHI_DEVICE_FLAG_LEAKED_KM))
		fu_device_inhibit(device, "leaked-km", "Provisioned with a leaked private key");
}

static void
fu_intel_mkhi_device_init(FuIntelMkhiDevice *self)
{
	fu_device_set_logical_id(FU_DEVICE(self), "MKHI");
	fu_device_set_name(FU_DEVICE(self), "BootGuard Configuration");
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_HOST_FIRMWARE_CHILD);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_ONLY_CHECKSUM);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_SET_FLAGS);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_COMPUTER);
	fu_device_register_private_flag(FU_DEVICE(self), FU_INTEL_MKHI_DEVICE_FLAG_LEAKED_KM);
	g_signal_connect(FWUPD_DEVICE(self),
			 "notify::private-flags",
			 G_CALLBACK(fu_intel_mkhi_device_version_notify_cb),
			 NULL);
}

static void
fu_intel_mkhi_device_class_init(FuIntelMkhiDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->setup = fu_intel_mkhi_device_setup;
}
