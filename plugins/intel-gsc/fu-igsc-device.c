/*
 * Copyright 2022 Intel, Inc
 * Copyright 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later OR Apache-2.0
 */

#include "config.h"

#include "fu-igsc-aux-device.h"
#include "fu-igsc-code-firmware.h"
#include "fu-igsc-device.h"
#include "fu-igsc-oprom-device.h"
#include "fu-igsc-struct.h"

struct _FuIgscDevice {
	FuHeciDevice parent_instance;
	gchar *project;
	guint32 hw_sku;
	guint16 subsystem_vendor;
	guint16 subsystem_model;
	gboolean oprom_code_devid_enforcement;
	guint8 svn_executing;
	guint8 svn_min_allowed;
};

#define FU_IGSC_DEVICE_FLAG_HAS_AUX   "has-aux"
#define FU_IGSC_DEVICE_FLAG_HAS_OPROM "has-oprom"

#define FU_IGSC_DEVICE_POWER_WRITE_TIMEOUT 1500	  /* ms */
#define FU_IGSC_DEVICE_MEI_WRITE_TIMEOUT   60000  /* 60 sec */
#define FU_IGSC_DEVICE_MEI_READ_TIMEOUT	   480000 /* 480 sec */

G_DEFINE_TYPE(FuIgscDevice, fu_igsc_device, FU_TYPE_HECI_DEVICE)

#define GSC_FWU_STATUS_SUCCESS			      0x0
#define GSC_FWU_STATUS_SIZE_ERROR		      0x5
#define GSC_FWU_STATUS_UPDATE_OPROM_INVALID_STRUCTURE 0x1035
#define GSC_FWU_STATUS_UPDATE_OPROM_SECTION_NOT_EXIST 0x1032
#define GSC_FWU_STATUS_INVALID_COMMAND		      0x8D
#define GSC_FWU_STATUS_INVALID_PARAMS		      0x85
#define GSC_FWU_STATUS_FAILURE			      0x9E

#define GSC_FWU_GET_CONFIG_FORMAT_VERSION 0x1

static void
fu_igsc_device_to_string(FuDevice *device, guint idt, GString *str)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(device);
	fwupd_codec_string_append(str, idt, "Project", self->project);
	fwupd_codec_string_append_hex(str, idt, "HwSku", self->hw_sku);
	fwupd_codec_string_append_hex(str, idt, "SubsystemVendor", self->subsystem_vendor);
	fwupd_codec_string_append_hex(str, idt, "SubsystemModel", self->subsystem_model);
	fwupd_codec_string_append_bool(str,
				       idt,
				       "OpromCodeDevidEnforcement",
				       self->oprom_code_devid_enforcement);
	fwupd_codec_string_append_hex(str, idt, "SvnExecuting", self->svn_executing);
	fwupd_codec_string_append_hex(str, idt, "SvnMinAllowed", self->svn_min_allowed);
}

gboolean
fu_igsc_device_get_oprom_code_devid_enforcement(FuIgscDevice *self)
{
	g_return_val_if_fail(FU_IS_IGSC_DEVICE(self), FALSE);
	return self->oprom_code_devid_enforcement;
}

guint16
fu_igsc_device_get_ssvid(FuIgscDevice *self)
{
	g_return_val_if_fail(FU_IS_IGSC_DEVICE(self), G_MAXUINT16);
	return self->subsystem_vendor;
}

guint16
fu_igsc_device_get_ssdid(FuIgscDevice *self)
{
	g_return_val_if_fail(FU_IS_IGSC_DEVICE(self), G_MAXUINT16);
	return self->subsystem_model;
}

static gboolean
fu_igsc_device_heci_validate_response_header(FuIgscDevice *self,
					     struct gsc_fwu_heci_response *resp_header,
					     FuIgscFwuHeciCommandId command_id,
					     GError **error)
{
	if (resp_header->header.command_id != command_id) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid command ID (%d): ",
			    resp_header->header.command_id);
		return FALSE;
	}
	if (!resp_header->header.is_response) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_DATA, "not a response");
		return FALSE;
	}
	if (resp_header->status != GSC_FWU_STATUS_SUCCESS) {
		const gchar *msg;
		switch (resp_header->status) {
		case GSC_FWU_STATUS_SIZE_ERROR:
			msg = "num of bytes to read/write/erase is bigger than partition size";
			break;
		case GSC_FWU_STATUS_UPDATE_OPROM_INVALID_STRUCTURE:
			msg = "wrong oprom signature";
			break;
		case GSC_FWU_STATUS_UPDATE_OPROM_SECTION_NOT_EXIST:
			msg = "update oprom section does not exists on flash";
			break;
		case GSC_FWU_STATUS_INVALID_COMMAND:
			msg = "invalid HECI message sent";
			break;
		case GSC_FWU_STATUS_INVALID_PARAMS:
			msg = "invalid command parameters";
			break;
		case GSC_FWU_STATUS_FAILURE:
		/* fall through */
		default:
			msg = "general firmware error";
			break;
		}
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "HECI message failed: %s [0x%x]: ",
			    msg,
			    resp_header->status);
		return FALSE;
	}
	if (resp_header->reserved != 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "HECI message response is leaking data");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_igsc_device_command(FuIgscDevice *self,
		       const guint8 *req_buf,
		       gsize req_bufsz,
		       guint8 *resp_buf,
		       gsize resp_bufsz,
		       GError **error)
{
	gsize resp_readsz = 0;
	if (!fu_mei_device_write(FU_MEI_DEVICE(self),
				 req_buf,
				 req_bufsz,
				 FU_IGSC_DEVICE_MEI_WRITE_TIMEOUT,
				 error))
		return FALSE;
	if (!fu_mei_device_read(FU_MEI_DEVICE(self),
				resp_buf,
				resp_bufsz,
				&resp_readsz,
				FU_IGSC_DEVICE_MEI_READ_TIMEOUT,
				error))
		return FALSE;
	if (resp_readsz != resp_bufsz) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "read 0x%x bytes but expected 0x%x",
			    (guint)resp_readsz,
			    (guint)resp_bufsz);
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_igsc_device_get_version_raw(FuIgscDevice *self,
			       FuIgscFwuHeciPartitionVersion partition,
			       guint8 *buf,
			       gsize bufsz,
			       GError **error)
{
	struct gsc_fwu_heci_version_req req = {.header.command_id =
						   FU_IGSC_FWU_HECI_COMMAND_ID_GET_IP_VERSION,
					       .partition = partition};
	guint8 res_buf[100];
	struct gsc_fwu_heci_version_resp *res = (struct gsc_fwu_heci_version_resp *)res_buf;

	if (!fu_igsc_device_command(self,
				    (const guint8 *)&req,
				    sizeof(req),
				    res_buf,
				    sizeof(struct gsc_fwu_heci_version_resp) + bufsz,
				    error)) {
		g_prefix_error(error, "invalid HECI message response: ");
		return FALSE;
	}
	if (!fu_igsc_device_heci_validate_response_header(self,
							  &res->response,
							  req.header.command_id,
							  error))
		return FALSE;
	if (res->partition != partition) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid HECI message response payload: 0x%x: ",
			    res->partition);
		return FALSE;
	}
	if (bufsz > 0 && res->version_length != bufsz) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid HECI message response version_length: 0x%x, expected 0x%x: ",
			    res->version_length,
			    (guint)bufsz);
		return FALSE;
	}
	if (buf != NULL) {
		if (!fu_memcpy_safe(buf,
				    bufsz,
				    0x0, /* dst */
				    res->version,
				    res->version_length,
				    0x0, /* src*/
				    res->version_length,
				    error)) {
			return FALSE;
		}
	}

	/* success */
	return TRUE;
}

gboolean
fu_igsc_device_get_aux_version(FuIgscDevice *self,
			       guint32 *oem_version,
			       guint16 *major_version,
			       guint16 *major_vcn,
			       GError **error)
{
	struct gsc_fw_data_heci_version_req req = {
	    .header.command_id = FU_IGSC_FWU_HECI_COMMAND_ID_GET_GFX_DATA_UPDATE_INFO};
	struct gsc_fw_data_heci_version_resp res = {0x0};

	if (!fu_igsc_device_command(self,
				    (const guint8 *)&req,
				    sizeof(req),
				    (guint8 *)&res,
				    sizeof(res),
				    error))
		return FALSE;
	if (!fu_igsc_device_heci_validate_response_header(self,
							  &res.response,
							  req.header.command_id,
							  error))
		return FALSE;

	/* success */
	*major_vcn = res.major_vcn;
	*major_version = res.major_version;
	if (res.oem_version_fitb_valid) {
		*oem_version = res.oem_version_fitb;
	} else {
		*oem_version = res.oem_version_nvm;
	}
	return TRUE;
}

static gboolean
fu_igsc_device_get_subsystem_ids(FuIgscDevice *self, GError **error)
{
	struct gsc_fwu_heci_get_subsystem_ids_message_req req = {
	    .header.command_id = FU_IGSC_FWU_HECI_COMMAND_ID_GET_SUBSYSTEM_IDS};
	struct gsc_fwu_heci_get_subsystem_ids_message_resp res = {0x0};

	if (!fu_igsc_device_command(self,
				    (const guint8 *)&req,
				    sizeof(req),
				    (guint8 *)&res,
				    sizeof(res),
				    error))
		return FALSE;
	if (!fu_igsc_device_heci_validate_response_header(self,
							  &res.response,
							  req.header.command_id,
							  error))
		return FALSE;

	/* success */
	self->subsystem_vendor = res.ssvid;
	self->subsystem_model = res.ssdid;
	return TRUE;
}

#define GSC_IFWI_TAG_SOC2_SKU_BIT 0x1
#define GSC_IFWI_TAG_SOC3_SKU_BIT 0x2
#define GSC_IFWI_TAG_SOC1_SKU_BIT 0x4

#define GSC_DG2_SKUID_SOC1 0
#define GSC_DG2_SKUID_SOC2 1
#define GSC_DG2_SKUID_SOC3 2

static gboolean
fu_igsc_device_get_config(FuIgscDevice *self, GError **error)
{
	struct gsc_fwu_heci_get_config_message_req req = {
	    .header.command_id = FU_IGSC_FWU_HECI_COMMAND_ID_GET_CONFIG,
	};
	struct gsc_fwu_heci_get_config_message_resp res = {0x0};

	if (!fu_igsc_device_command(self,
				    (const guint8 *)&req,
				    sizeof(req),
				    (guint8 *)&res,
				    sizeof(res),
				    error)) {
		g_prefix_error(error, "invalid HECI message response: ");
		return FALSE;
	}
	if (!fu_igsc_device_heci_validate_response_header(self,
							  &res.response,
							  req.header.command_id,
							  error))
		return FALSE;
	if (res.format_version != GSC_FWU_GET_CONFIG_FORMAT_VERSION) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid config version 0x%x, expected 0x%x",
			    res.format_version,
			    (guint)GSC_FWU_GET_CONFIG_FORMAT_VERSION);
		return FALSE;
	}

	/* convert to firmware bit mask for easier comparison */
	if (res.hw_sku == GSC_DG2_SKUID_SOC1) {
		self->hw_sku = GSC_IFWI_TAG_SOC1_SKU_BIT;
	} else if (res.hw_sku == GSC_DG2_SKUID_SOC3) {
		self->hw_sku = GSC_IFWI_TAG_SOC3_SKU_BIT;
	} else if (res.hw_sku == GSC_DG2_SKUID_SOC2) {
		self->hw_sku = GSC_IFWI_TAG_SOC2_SKU_BIT;
	} else {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "invalid hw sku 0x%x, expected 0..2",
			    res.hw_sku);
		return FALSE;
	}

	self->oprom_code_devid_enforcement = res.oprom_code_devid_enforcement;

	/* success */
	return TRUE;
}

static gboolean
fu_igsc_device_setup(FuDevice *device, GError **error)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(device);
	FuContext *ctx = fu_device_get_context(FU_DEVICE(self));
	g_autofree gchar *version = NULL;
	g_autoptr(GByteArray) fw_code_version = fu_struct_igsc_fw_version_new();

	/* connect to MCA interface */
	if (!fu_mei_device_connect(FU_MEI_DEVICE(self), FU_HECI_DEVICE_UUID_MCHI2, 0, error)) {
		g_prefix_error(error, "failed to connect: ");
		return FALSE;
	}
	if (!fu_heci_device_arbh_svn_get_info(FU_HECI_DEVICE(self),
					      FU_MKHI_ARBH_SVN_INFO_ENTRY_USAGE_ID_CSE_RBE,
					      &self->svn_executing,
					      &self->svn_min_allowed,
					      error)) {
		g_prefix_error(error, "failed to get ARBH SVN: ");
		return FALSE;
	}
	if (!fu_udev_device_reopen(FU_UDEV_DEVICE(self), error))
		return FALSE;

	/* now connect to fwupdate interface */
	if (!fu_mei_device_connect(FU_MEI_DEVICE(self), FU_HECI_DEVICE_UUID_FWUPDATE, 0, error)) {
		g_prefix_error(error, "failed to connect: ");
		return FALSE;
	}

	/* get current version */
	if (!fu_igsc_device_get_version_raw(self,
					    FU_IGSC_FWU_HECI_PARTITION_VERSION_GFX_FW,
					    fw_code_version->data,
					    fw_code_version->len,
					    error)) {
		g_prefix_error(error, "cannot get fw version: ");
		return FALSE;
	}
	self->project = fu_struct_igsc_fw_version_get_project(fw_code_version);
	if (fu_device_has_private_flag(device, FU_IGSC_DEVICE_FLAG_IS_WEDGED)) {
		version = g_strdup("0.0");
	} else {
		version = g_strdup_printf("%u.%u",
					  fu_struct_igsc_fw_version_get_hotfix(fw_code_version),
					  fu_struct_igsc_fw_version_get_build(fw_code_version));
	}
	fu_device_set_version(device, version);

	/* get hardware SKU if supported */
	if (g_strcmp0(self->project, "DG02") == 0) {
		if (!fu_igsc_device_get_config(self, error)) {
			g_prefix_error(error, "cannot get SKU: ");
			return FALSE;
		}
	}

	/* allow vendors to differentiate their products */
	if (!fu_igsc_device_get_subsystem_ids(self, error))
		return FALSE;
	if (self->subsystem_vendor != 0x0 && self->subsystem_model != 0x0) {
		g_autofree gchar *subsys =
		    g_strdup_printf("%04X%04X", self->subsystem_vendor, self->subsystem_model);
		fu_device_add_instance_str(device, "SUBSYS", subsys);
	}

	/* some devices have children */
	if (fu_device_has_private_flag(device, FU_IGSC_DEVICE_FLAG_HAS_AUX)) {
		g_autoptr(FuIgscAuxDevice) device_child = fu_igsc_aux_device_new(ctx);
		fu_device_add_child(FU_DEVICE(self), FU_DEVICE(device_child));
	}
	if (fu_device_has_private_flag(device, FU_IGSC_DEVICE_FLAG_HAS_OPROM)) {
		g_autoptr(FuIgscOpromDevice) device_code = NULL;
		g_autoptr(FuIgscOpromDevice) device_data = NULL;
		device_code =
		    fu_igsc_oprom_device_new(ctx, FU_IGSC_FWU_HECI_PAYLOAD_TYPE_OPROM_CODE);
		device_data =
		    fu_igsc_oprom_device_new(ctx, FU_IGSC_FWU_HECI_PAYLOAD_TYPE_OPROM_DATA);
		fu_device_add_child(FU_DEVICE(self), FU_DEVICE(device_code));
		fu_device_add_child(FU_DEVICE(self), FU_DEVICE(device_data));
	}

	/* success */
	return TRUE;
}

/* @line is indexed from 1 */
static gboolean
fu_igsc_device_get_fw_status(FuIgscDevice *self, guint line, guint32 *fw_status, GError **error)
{
	guint64 tmp64 = 0;
	g_autofree gchar *tmp = NULL;
	g_autofree gchar *hex = NULL;

	g_return_val_if_fail(line >= 1, FALSE);

	/* read value and convert to hex */
	tmp = fu_mei_device_get_fw_status(FU_MEI_DEVICE(self), line - 1, error);
	if (tmp == NULL) {
		g_prefix_error(error, "device is corrupted: ");
		return FALSE;
	}
	hex = g_strdup_printf("0x%s", tmp);
	if (!fu_strtoull(hex, &tmp64, 0x1, G_MAXUINT32 - 0x1, FU_INTEGER_BASE_AUTO, error)) {
		g_prefix_error(error, "fw_status %s is invalid: ", tmp);
		return FALSE;
	}

	/* success */
	if (fw_status != NULL)
		*fw_status = tmp64;
	return TRUE;
}

static gboolean
fu_igsc_device_probe(FuDevice *device, GError **error)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(device);
	g_autofree gchar *prop_wedged = NULL;

	/* check firmware status */
	if (!fu_igsc_device_get_fw_status(self, 1, NULL, error))
		return FALSE;

	/* device is wedged and needs recovery */
	prop_wedged = fu_udev_device_read_property(FU_UDEV_DEVICE(device), "WEDGED", NULL);
	if (g_strcmp0(prop_wedged, "unknown") == 0) {
		g_autofree gchar *attr_survivability_mode = NULL;
		attr_survivability_mode =
		    fu_udev_device_read_sysfs(FU_UDEV_DEVICE(self),
					      "attr_survivability_mode",
					      FU_UDEV_DEVICE_ATTR_READ_TIMEOUT_DEFAULT,
					      error);
		if (attr_survivability_mode == NULL) {
			g_prefix_error(error, "cannot get survivability_mode for WEDGED=unknown: ");
			return FALSE;
		}
		g_debug("survivability_mode: %s", attr_survivability_mode);
		fu_device_add_private_flag(device, FU_IGSC_DEVICE_FLAG_IS_WEDGED);
	}

	/* add extra instance IDs */
	fu_device_add_instance_str(device,
				   "PART",
				   fu_device_has_private_flag(device, FU_IGSC_DEVICE_FLAG_IS_WEDGED)
				       ? "FWCODE_RECOVERY"
				       : "FWCODE");
	if (!fu_device_build_instance_id(device, error, "PCI", "VEN", "DEV", "PART", NULL))
		return FALSE;
	return fu_device_build_instance_id(device,
					   error,
					   "PCI",
					   "VEN",
					   "DEV",
					   "SUBSYS",
					   "PART",
					   NULL);
}

static FuFirmware *
fu_igsc_device_prepare_firmware(FuDevice *device,
				GInputStream *stream,
				FuProgress *progress,
				FuFirmwareParseFlags flags,
				GError **error)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(device);
	g_autoptr(FuFirmware) firmware = fu_igsc_code_firmware_new();

	/* check project code */
	if (!fu_firmware_parse_stream(firmware, stream, 0x0, flags, error))
		return NULL;
	if (g_strcmp0(self->project, fu_firmware_get_id(firmware)) != 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "firmware is for a different project, got %s, expected %s",
			    fu_firmware_get_id(firmware),
			    self->project);
		return NULL;
	}
	if (self->hw_sku != fu_igsc_code_firmware_get_hw_sku(FU_IGSC_CODE_FIRMWARE(firmware))) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "firmware is for a different SKU, got 0x%x, expected 0x%x",
			    fu_igsc_code_firmware_get_hw_sku(FU_IGSC_CODE_FIRMWARE(firmware)),
			    self->hw_sku);
		return NULL;
	}

	/* success */
	return g_steal_pointer(&firmware);
}

static gboolean
fu_igsc_device_update_end(FuIgscDevice *self, GError **error)
{
	struct gsc_fwu_heci_end_req req = {.header.command_id = FU_IGSC_FWU_HECI_COMMAND_ID_END};
	return fu_mei_device_write(FU_MEI_DEVICE(self),
				   (const guint8 *)&req,
				   sizeof(req),
				   FU_IGSC_DEVICE_MEI_WRITE_TIMEOUT,
				   error);
}

static gboolean
fu_igsc_device_update_data(FuIgscDevice *self, const guint8 *data, gsize length, GError **error)
{
	struct gsc_fwu_heci_data_req req = {.header.command_id = FU_IGSC_FWU_HECI_COMMAND_ID_DATA,
					    .data_length = length};
	struct gsc_fwu_heci_data_resp res = {0x0};
	g_autoptr(GByteArray) buf = g_byte_array_new();

	g_byte_array_append(buf, (const guint8 *)&req, sizeof(req));
	g_byte_array_append(buf, data, length);
	if (!fu_igsc_device_command(self, buf->data, buf->len, (guint8 *)&res, sizeof(res), error))
		return FALSE;
	return fu_igsc_device_heci_validate_response_header(self,
							    &res.response,
							    req.header.command_id,
							    error);
}

static gboolean
fu_igsc_device_update_start(FuIgscDevice *self,
			    guint32 payload_type,
			    GBytes *fw_info,
			    GInputStream *fw,
			    GError **error)
{
	gsize streamsz = 0;
	struct gsc_fwu_heci_start_req req = {.header.command_id = FU_IGSC_FWU_HECI_COMMAND_ID_START,
					     .payload_type = payload_type,
					     .flags = {0}};
	struct gsc_fwu_heci_start_resp res = {0x0};
	g_autoptr(GByteArray) buf = g_byte_array_new();

	if (!fu_input_stream_size(fw, &streamsz, error))
		return FALSE;
	req.update_img_length = streamsz;

	g_byte_array_append(buf, (const guint8 *)&req, sizeof(req));
	if (fw_info != NULL)
		fu_byte_array_append_bytes(buf, fw_info);
	if (!fu_igsc_device_command(self, buf->data, buf->len, (guint8 *)&res, sizeof(res), error))
		return FALSE;
	return fu_igsc_device_heci_validate_response_header(self,
							    &res.response,
							    req.header.command_id,
							    error);
}

static gboolean
fu_igsc_device_no_update(FuIgscDevice *self, GError **error)
{
	struct gsc_fwu_heci_no_update_req req = {.header.command_id =
						     FU_IGSC_FWU_HECI_COMMAND_ID_NO_UPDATE};
	return fu_mei_device_write(FU_MEI_DEVICE(self),
				   (const guint8 *)&req,
				   sizeof(req),
				   FU_IGSC_DEVICE_MEI_WRITE_TIMEOUT,
				   error);
}

static gboolean
fu_igsc_device_write_chunks(FuIgscDevice *self,
			    FuChunkArray *chunks,
			    FuProgress *progress,
			    GError **error)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_steps(progress, fu_chunk_array_length(chunks));
	for (guint i = 0; i < fu_chunk_array_length(chunks); i++) {
		g_autoptr(FuChunk) chk = NULL;

		/* prepare chunk */
		chk = fu_chunk_array_index(chunks, i, error);
		if (chk == NULL)
			return FALSE;
		if (!fu_igsc_device_update_data(self,
						fu_chunk_get_data(chk),
						fu_chunk_get_data_sz(chk),
						error)) {
			g_prefix_error(error,
				       "failed on chunk %u (@0x%x): ",
				       i,
				       (guint)fu_chunk_get_address(chk));
			return FALSE;
		}
		fu_progress_step_done(progress);
	}
	return TRUE;
}

/* the expectation is that it will fail eventually */
static gboolean
fu_igsc_device_wait_for_reset(FuIgscDevice *self, GError **error)
{
	g_autoptr(GByteArray) fw_code_version = fu_struct_igsc_fw_version_new();
	for (guint i = 0; i < 20; i++) {
		if (!fu_igsc_device_get_version_raw(self,
						    FU_IGSC_FWU_HECI_PARTITION_VERSION_GFX_FW,
						    fw_code_version->data,
						    fw_code_version->len,
						    NULL))
			return TRUE;
		fu_device_sleep(FU_DEVICE(self), 100);
	}
	g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_TIMED_OUT, "device did not reset");
	return FALSE;
}

static gboolean
fu_igsc_device_reconnect_cb(FuDevice *self, gpointer user_data, GError **error)
{
	return fu_mei_device_connect(FU_MEI_DEVICE(self), FU_HECI_DEVICE_UUID_FWUPDATE, 0, error);
}

gboolean
fu_igsc_device_write_blob(FuIgscDevice *self,
			  FuIgscFwuHeciPayloadType payload_type,
			  GBytes *fw_info,
			  GInputStream *fw,
			  FuProgress *progress,
			  GError **error)
{
	gboolean cp_mode;
	guint32 sts5 = 0;
	gsize payloadsz = fu_mei_device_get_max_msg_length(FU_MEI_DEVICE(self)) -
			  sizeof(struct gsc_fwu_heci_data_req);
	g_autoptr(FuChunkArray) chunks = NULL;

	/* progress */
	if (payload_type == FU_IGSC_FWU_HECI_PAYLOAD_TYPE_GFX_FW) {
		fu_progress_set_id(progress, G_STRLOC);
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 1, "get-status");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 1, "update-start");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 50, "write-chunks");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 1, "update-end");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 1, "wait-for-reboot");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 46, "reconnect");
	} else {
		fu_progress_set_id(progress, G_STRLOC);
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 1, "get-status");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 1, "update-start");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 96, "write-chunks");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 1, "update-end");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 1, "wait-for-reboot");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 0, "reconnect");
	}

	/* need to get the new version in a loop? */
	if (!fu_igsc_device_get_fw_status(self, 5, &sts5, error))
		return FALSE;
	cp_mode = (sts5 & HECI1_CSE_FS_MODE_MASK) == HECI1_CSE_FS_CP_MODE;
	fu_progress_step_done(progress);

	/* start */
	if (!fu_igsc_device_update_start(self, payload_type, fw_info, fw, error)) {
		g_prefix_error(error, "failed to start: ");
		return FALSE;
	}
	fu_progress_step_done(progress);

	/* data */
	chunks = fu_chunk_array_new_from_stream(fw,
						FU_CHUNK_ADDR_OFFSET_NONE,
						FU_CHUNK_PAGESZ_NONE,
						payloadsz,
						error);
	if (chunks == NULL)
		return FALSE;
	if (!fu_igsc_device_write_chunks(self, chunks, fu_progress_get_child(progress), error))
		return FALSE;
	fu_progress_step_done(progress);

	/* stop */
	if (!fu_igsc_device_update_end(self, error)) {
		g_prefix_error(error, "failed to end: ");
		return FALSE;
	}
	fu_progress_step_done(progress);

	/* detect a firmware reboot */
	if (payload_type == FU_IGSC_FWU_HECI_PAYLOAD_TYPE_GFX_FW ||
	    payload_type == FU_IGSC_FWU_HECI_PAYLOAD_TYPE_FWDATA) {
		if (!fu_igsc_device_wait_for_reset(self, error))
			return FALSE;
	}
	fu_progress_step_done(progress);

	/* after Gfx FW update there is a FW reset so driver reconnect is needed */
	if (payload_type == FU_IGSC_FWU_HECI_PAYLOAD_TYPE_GFX_FW) {
		if (cp_mode) {
			if (!fu_igsc_device_wait_for_reset(self, error))
				return FALSE;
		}
		if (!fu_device_retry_full(FU_DEVICE(self),
					  fu_igsc_device_reconnect_cb,
					  200,
					  300 /* ms */,
					  NULL,
					  error))
			return FALSE;
		if (!fu_igsc_device_no_update(self, error)) {
			g_prefix_error(error, "failed to send no-update: ");
			return FALSE;
		}
		fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	}
	fu_progress_step_done(progress);

	/* success */
	return TRUE;
}

static gboolean
fu_igsc_device_write_firmware(FuDevice *device,
			      FuFirmware *firmware,
			      FuProgress *progress,
			      FwupdInstallFlags flags,
			      GError **error)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(device);
	g_autoptr(GBytes) fw_info = NULL;
	g_autoptr(GInputStream) stream_payload = NULL;

	/* get image, and install on ourself */
	fw_info =
	    fu_firmware_get_image_by_idx_bytes(firmware, FU_IFWI_FPT_FIRMWARE_IDX_INFO, error);
	if (fw_info == NULL)
		return FALSE;
	stream_payload =
	    fu_firmware_get_image_by_idx_stream(firmware, FU_IFWI_FPT_FIRMWARE_IDX_FWIM, error);
	if (stream_payload == NULL)
		return FALSE;
	if (!fu_igsc_device_write_blob(self,
				       FU_IGSC_FWU_HECI_PAYLOAD_TYPE_GFX_FW,
				       fw_info,
				       stream_payload,
				       progress,
				       error))
		return FALSE;

	/* restart */
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	return TRUE;
}

static gboolean
fu_igsc_device_set_pci_power_policy(FuIgscDevice *self, const gchar *val, GError **error)
{
	g_autoptr(FuDevice) parent = NULL;

	/* get PCI parent */
	parent = fu_device_get_backend_parent_with_subsystem(FU_DEVICE(self), "pci", error);
	if (parent == NULL)
		return FALSE;
	return fu_udev_device_write_sysfs(FU_UDEV_DEVICE(parent),
					  "power/control",
					  val,
					  FU_IGSC_DEVICE_POWER_WRITE_TIMEOUT,
					  error);
}

static gboolean
fu_igsc_device_prepare(FuDevice *device,
		       FuProgress *progress,
		       FwupdInstallFlags flags,
		       GError **error)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(device);
	return fu_igsc_device_set_pci_power_policy(self, "on", error);
}

static gboolean
fu_igsc_device_cleanup(FuDevice *device,
		       FuProgress *progress,
		       FwupdInstallFlags flags,
		       GError **error)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(device);
	return fu_igsc_device_set_pci_power_policy(self, "auto", error);
}

static void
fu_igsc_device_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "prepare-fw");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 1, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 96, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 2, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 1, "reload");
}

static void
fu_igsc_device_init(FuIgscDevice *self)
{
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_REQUIRE_AC);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_INSTALL_PARENT_FIRST);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_SAVE_INTO_BACKUP_REMOTE);
	fu_device_set_summary(FU_DEVICE(self), "Discrete Graphics Card");
	fu_device_add_protocol(FU_DEVICE(self), "com.intel.gsc");
	fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_GPU);
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_PAIR);
	fu_device_set_remove_delay(FU_DEVICE(self), 60000);
	fu_device_register_private_flag(FU_DEVICE(self), FU_IGSC_DEVICE_FLAG_HAS_AUX);
	fu_device_register_private_flag(FU_DEVICE(self), FU_IGSC_DEVICE_FLAG_HAS_OPROM);
	fu_device_register_private_flag(FU_DEVICE(self), FU_IGSC_DEVICE_FLAG_IS_WEDGED);
}

static void
fu_igsc_device_finalize(GObject *object)
{
	FuIgscDevice *self = FU_IGSC_DEVICE(object);

	g_free(self->project);

	G_OBJECT_CLASS(fu_igsc_device_parent_class)->finalize(object);
}

static void
fu_igsc_device_class_init(FuIgscDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fu_igsc_device_finalize;
	device_class->set_progress = fu_igsc_device_set_progress;
	device_class->to_string = fu_igsc_device_to_string;
	device_class->setup = fu_igsc_device_setup;
	device_class->probe = fu_igsc_device_probe;
	device_class->prepare = fu_igsc_device_prepare;
	device_class->cleanup = fu_igsc_device_cleanup;
	device_class->prepare_firmware = fu_igsc_device_prepare_firmware;
	device_class->write_firmware = fu_igsc_device_write_firmware;
}
