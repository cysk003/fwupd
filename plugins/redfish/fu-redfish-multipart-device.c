/*
 * Copyright 2021 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <curl/curl.h>

#include "fu-redfish-device.h"
#include "fu-redfish-multipart-device.h"
#include "fu-redfish-request.h"

struct _FuRedfishMultipartDevice {
	FuRedfishDevice parent_instance;
	gchar *apply_time;
};

G_DEFINE_TYPE(FuRedfishMultipartDevice, fu_redfish_multipart_device, FU_TYPE_REDFISH_DEVICE)

G_DEFINE_AUTOPTR_CLEANUP_FUNC(curl_mime, curl_mime_free)

static void
fu_redfish_multipart_device_to_string(FuDevice *device, guint idt, GString *str)
{
	FuRedfishMultipartDevice *self = FU_REDFISH_MULTIPART_DEVICE(device);
	fwupd_codec_string_append(str, idt, "ApplyTime", self->apply_time);
}

void
fu_redfish_multipart_device_set_apply_time(FuRedfishMultipartDevice *self, const gchar *apply_time)
{
	g_return_if_fail(FU_IS_REDFISH_MULTIPART_DEVICE(self));
	g_return_if_fail(apply_time != NULL);
	if (g_strcmp0(self->apply_time, apply_time) == 0)
		return;
	g_free(self->apply_time);
	self->apply_time = g_strdup(apply_time);
}

static GString *
fu_redfish_multipart_device_get_parameters(FuRedfishMultipartDevice *self)
{
	g_autoptr(GString) str = g_string_new(NULL);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) json_generator = json_generator_new();
	g_autoptr(JsonNode) json_root = NULL;

	/* create header */
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "Targets");
	json_builder_begin_array(builder);
	if (!fu_device_has_private_flag(FU_DEVICE(self), FU_REDFISH_DEVICE_FLAG_WILDCARD_TARGETS)) {
		const gchar *logical_id = fu_device_get_logical_id(FU_DEVICE(self));
		json_builder_add_string_value(builder, logical_id);
	}
	json_builder_end_array(builder);
	if (self->apply_time != NULL) {
		json_builder_set_member_name(builder, "@Redfish.OperationApplyTime");
		json_builder_add_string_value(builder, self->apply_time);
	}
	json_builder_end_object(builder);

	/* export as a string */
	json_root = json_builder_get_root(builder);
	json_generator_set_pretty(json_generator, TRUE);
	json_generator_set_root(json_generator, json_root);
	json_generator_to_gstring(json_generator, str);
	return g_steal_pointer(&str);
}

static size_t
fu_redfish_multipart_device_location_headers_callback(char *ptr,
						      size_t size,
						      size_t nmemb,
						      void *location)
{
	char **loc_str = (char **)location;
	if ((size * nmemb) > 16 && g_ascii_strncasecmp(ptr, "Location:", 9) == 0) {
		/* The string also includes \r\n at the end */
		*loc_str = g_strndup(ptr + 10, (size * nmemb) - 12);
	}
	return size * nmemb;
}

static gboolean
fu_redfish_multipart_device_write_firmware(FuDevice *device,
					   FuFirmware *firmware,
					   FuProgress *progress,
					   FwupdInstallFlags flags,
					   GError **error)
{
	FuRedfishMultipartDevice *self = FU_REDFISH_MULTIPART_DEVICE(device);
	FuRedfishBackend *backend = fu_redfish_device_get_backend(FU_REDFISH_DEVICE(self));
	CURL *curl;
	JsonObject *json_obj;
	curl_mimepart *part;
	const gchar *location;
	g_autoptr(curl_mime) mime = NULL;
	g_autoptr(FuRedfishRequest) request = NULL;
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(GString) params = NULL;

	/* get default image */
	fw = fu_firmware_get_bytes(firmware, error);
	if (fw == NULL)
		return FALSE;

	/* create the multipart request */
	request = fu_redfish_backend_request_new(backend);
	curl = fu_redfish_request_get_curl(request);
	mime = curl_mime_init(curl);

	params = fu_redfish_multipart_device_get_parameters(self);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "UpdateParameters");
	(void)curl_mime_type(part, "application/json");
	(void)curl_mime_data(part, params->str, CURL_ZERO_TERMINATED);
	g_debug("request: %s", params->str);

	part = curl_mime_addpart(mime);
	curl_mime_name(part, "UpdateFile");
	(void)curl_mime_type(part, "application/octet-stream");
	(void)curl_mime_filename(part, fu_firmware_get_filename(firmware));

	(void)curl_mime_data(part, g_bytes_get_data(fw, NULL), g_bytes_get_size(fw));

	(void)curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	(void)curl_easy_setopt(curl,
			       CURLOPT_HEADERFUNCTION,
			       fu_redfish_multipart_device_location_headers_callback);
	(void)curl_easy_setopt(fu_redfish_request_get_curl(request), CURLOPT_HEADERDATA, &location);

	fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_WRITE);
	if (!fu_redfish_request_perform(request,
					fu_redfish_backend_get_push_uri_path(backend),
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;

	if (fu_redfish_request_get_status_code(request) != 202) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_FILE,
			    "failed to upload: %li",
			    fu_redfish_request_get_status_code(request));
		return FALSE;
	}

	/* prefer the header, otherwise fall back to the response */
	if (location == NULL || g_utf8_strlen(location, 1) == 0) {
		json_obj = fu_redfish_request_get_json_object(request);
		if (json_object_has_member(json_obj, "TaskMonitor")) {
			const gchar *tmp = json_object_get_string_member(json_obj, "TaskMonitor");
			g_debug("task manager for cleanup is %s", tmp);
		}

		/* poll the task for progress */
		if (!json_object_has_member(json_obj, "@odata.id")) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "no task returned for %s",
				    fu_redfish_backend_get_push_uri_path(backend));
			return FALSE;
		}
		location = json_object_get_string_member(json_obj, "@odata.id");
	}
	return fu_redfish_device_poll_task(FU_REDFISH_DEVICE(self), location, progress, error);
}

static void
fu_redfish_multipart_device_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "prepare-fw");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 100, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 0, "reload");
}

static void
fu_redfish_multipart_device_init(FuRedfishMultipartDevice *self)
{
	fu_redfish_multipart_device_set_apply_time(self, "Immediate");
	fu_device_set_summary(FU_DEVICE(self), "Redfish multipart device");
}

static void
fu_redfish_multipart_device_finalize(GObject *obj)
{
	FuRedfishMultipartDevice *self = FU_REDFISH_MULTIPART_DEVICE(obj);
	g_free(self->apply_time);
	G_OBJECT_CLASS(fu_redfish_multipart_device_parent_class)->finalize(obj);
}

static void
fu_redfish_multipart_device_class_init(FuRedfishMultipartDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = fu_redfish_multipart_device_finalize;
	device_class->to_string = fu_redfish_multipart_device_to_string;
	device_class->write_firmware = fu_redfish_multipart_device_write_firmware;
	device_class->set_progress = fu_redfish_multipart_device_set_progress;
}
