/*
 * Copyright 2024 NVIDIA Corporation
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <curl/curl.h>

#include "fu-redfish-backend.h"
#include "fu-redfish-firmware-update-service-device.h"
#include "fu-redfish-request.h"

struct _FuRedfishFirmwareUpdateServiceDevice {
	FuRedfishDevice parent_instance;
	gchar *http_push_uri_path;
};

G_DEFINE_TYPE(FuRedfishFirmwareUpdateServiceDevice,
	      fu_redfish_firmware_update_service_device,
	      FU_TYPE_REDFISH_DEVICE)

static gboolean
fu_redfish_firmware_update_service_device_probe(FuDevice *device, GError **error)
{
	FuRedfishFirmwareUpdateServiceDevice *self =
	    FU_REDFISH_FIRMWARE_UPDATE_SERVICE_DEVICE(device);
	FuRedfishBackend *backend;
	const gchar *vendor;
	const gchar *http_push_uri;
	g_autoptr(FwupdJsonObject) json_obj = NULL;
	g_autoptr(FuRedfishRequest) request = NULL;

	backend = fu_redfish_device_get_backend(FU_REDFISH_DEVICE(device), error);
	if (backend == NULL)
		return FALSE;

	/* GET /redfish/v1/UpdateService to discover HttpPushUri */
	request = fu_redfish_backend_request_new(backend);
	if (!fu_redfish_request_perform(request,
					"/redfish/v1/UpdateService",
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);
	http_push_uri = fwupd_json_object_get_string(json_obj, "HttpPushUri", NULL);
	if (http_push_uri != NULL) {
		g_free(self->http_push_uri_path);
		self->http_push_uri_path = g_strdup(http_push_uri);
		g_debug("UpdateService: using HttpPushUri %s", self->http_push_uri_path);
	} else {
		/* fallback to whatever the backend selected */
		self->http_push_uri_path = g_strdup(fu_redfish_backend_get_push_uri_path(backend));
		g_debug("UpdateService: HttpPushUri not found, falling back to push_uri_path %s",
			self->http_push_uri_path);
	}

	/* set IDs */
	fu_device_set_physical_id(device, "Redfish-UpdateService");
	fu_device_set_logical_id(device, self->http_push_uri_path);
	fu_device_set_backend_id(device, "UpdateService");

	/* set device properties */
	fu_device_set_name(device, "Redfish Update Service");
	fu_device_set_summary(device, "Redfish firmware update service endpoint");

	/* set vendor and build instance IDs */
	vendor = fu_redfish_backend_get_vendor(backend);
	if (vendor != NULL) {
		g_autofree gchar *vendor_upper = g_ascii_strup(vendor, -1);
		g_autofree gchar *instance_id = NULL;
		g_strdelimit(vendor_upper, " ", '_');
		fu_device_build_vendor_id(device, "REDFISH", vendor_upper);
		/* build "REDFISH\VENDOR_<VENDOR>&UPDATESERVICE" directly to get the correct GUID */
		instance_id = g_strdup_printf("REDFISH\\VENDOR_%s&UPDATESERVICE", vendor_upper);
		fu_device_add_instance_id(device, instance_id);
	}

	/* flags */
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_private_flag(device, FU_DEVICE_PRIVATE_FLAG_NO_VERSION_EXPECTED);

	return TRUE;
}

static gboolean
fu_redfish_firmware_update_service_device_write_firmware(FuDevice *device,
							 FuFirmware *firmware,
							 FuProgress *progress,
							 FwupdInstallFlags flags,
							 GError **error)
{
	FuRedfishFirmwareUpdateServiceDevice *self =
	    FU_REDFISH_FIRMWARE_UPDATE_SERVICE_DEVICE(device);
	FuRedfishBackend *backend;
	CURL *curl;
	const gchar *location;
	g_autoptr(FwupdJsonObject) json_obj = NULL;
	g_autoptr(FuRedfishRequest) request = NULL;
	g_autoptr(GBytes) fw = NULL;

	/* get default image */
	fw = fu_firmware_get_bytes(firmware, error);
	if (fw == NULL)
		return FALSE;

	/* if the previous install loop already succeeded, skip the duplicate POST */
	if (fu_device_get_update_state(device) == FWUPD_UPDATE_STATE_SUCCESS) {
		g_debug("skipping duplicate write_firmware call: update already succeeded");
		fu_progress_finished(progress);
		return TRUE;
	}

	/* POST data directly to HttpPushUri without specifying targets */
	backend = fu_redfish_device_get_backend(FU_REDFISH_DEVICE(self), error);
	if (backend == NULL)
		return FALSE;
	if (self->http_push_uri_path == NULL) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "no HttpPushUri");
		return FALSE;
	}
	request = fu_redfish_backend_request_new(backend);
	curl = fu_redfish_request_get_curl(request);
	(void)curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
	(void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, g_bytes_get_data(fw, NULL));
	(void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)g_bytes_get_size(fw));
	fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_WRITE);
	if (!fu_redfish_request_perform(request,
					self->http_push_uri_path,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;

	/* poll the task for progress */
	json_obj = fu_redfish_request_get_json_object(request);
	location = fwupd_json_object_get_string(json_obj, "@odata.id", error);
	if (location == NULL) {
		g_prefix_error(error, "no task returned for %s: ", self->http_push_uri_path);
		return FALSE;
	}
	return fu_redfish_device_poll_task(FU_REDFISH_DEVICE(self), location, progress, error);
}

static void
fu_redfish_firmware_update_service_device_set_progress(FuDevice *device, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "prepare-firmware");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 94, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 6, "reload");
}

static void
fu_redfish_firmware_update_service_device_init(FuRedfishFirmwareUpdateServiceDevice *self)
{
}

static void
fu_redfish_firmware_update_service_device_finalize(GObject *obj)
{
	FuRedfishFirmwareUpdateServiceDevice *self =
	    FU_REDFISH_FIRMWARE_UPDATE_SERVICE_DEVICE(obj);
	g_free(self->http_push_uri_path);
	G_OBJECT_CLASS(fu_redfish_firmware_update_service_device_parent_class)->finalize(obj);
}

static void
fu_redfish_firmware_update_service_device_class_init(
    FuRedfishFirmwareUpdateServiceDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = fu_redfish_firmware_update_service_device_finalize;
	device_class->probe = fu_redfish_firmware_update_service_device_probe;
	device_class->write_firmware = fu_redfish_firmware_update_service_device_write_firmware;
	device_class->set_progress = fu_redfish_firmware_update_service_device_set_progress;
}

FuRedfishFirmwareUpdateServiceDevice *
fu_redfish_firmware_update_service_device_new(FuContext *ctx, FuRedfishBackend *backend)
{
	return FU_REDFISH_FIRMWARE_UPDATE_SERVICE_DEVICE(
	    g_object_new(FU_TYPE_REDFISH_FIRMWARE_UPDATE_SERVICE_DEVICE,
			 "context",
			 ctx,
			 "backend",
			 backend,
			 "member",
			 NULL,
			 NULL));
}
