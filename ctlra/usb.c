#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "impl.h"

#include "libusb.h"

#define USB_PATH_MAX 256

/* From cltra.c */
extern int ctlra_impl_get_id_by_vid_pid(uint32_t vid, uint32_t pid);
extern int ctlra_impl_accept_dev(struct ctlra_t *ctlra, int dev_id);
extern int ctlra_impl_dev_get_by_vid_pid(struct ctlra_t *ctlra, int32_t vid,
					 int32_t pid, struct ctlra_dev_t **out_dev);

static int ctlra_usb_impl_get_serial(struct libusb_device_handle *handle,
				     uint8_t desc_serial, uint8_t *buffer,
				     uint32_t buf_size)
{
	if (desc_serial > 0) {
		int ret = libusb_get_string_descriptor_ascii(handle,
							     desc_serial,
							     buffer,
							     buf_size);
		if (ret < 0)
			return -1;
	}
	return 0;
}

static int ctlra_usb_impl_hotplug_cb(libusb_context *ctx,
                                     libusb_device *dev,
                                     libusb_hotplug_event event,
                                     void *user_data)
{
	int ret;
	struct ctlra_t *ctlra = user_data;
	struct libusb_device_descriptor desc;
	ret = libusb_get_device_descriptor(dev, &desc);
	if(ret != LIBUSB_SUCCESS) {
		printf("Error getting device descriptor\n");
		return -1;
	}
	if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
		/* Quirks:
		 * If a device is unplugged, usually the libusb read/write
		 * will fail, causing the device to be banished, and then
		 * cleaned up and removed automatically by Ctlra. The
		 * exception is devices that read /dev/hidrawX manually,
		 * because they return -1 if no data is available or there
		 * is an error reading the file descriptor.
		 *
		 * The solution used here it to use libusb to detect the
		 * removal of the device, and then banish the ctlra_dev_t
		 * instance if it matches the device */
#if 0
		printf("Device removed: %04x:%04x, ctlra %p\n",
		       desc.idVendor, desc.idProduct, user_data);
#endif
		/* NI Maschine Mikro MK2 */
		if(desc.idVendor == 0x17cc && desc.idProduct == 0x1200) {
			struct ctlra_dev_t *ni_mm;
			int err = ctlra_impl_dev_get_by_vid_pid(ctlra,
								0x17cc,
								0x1200,
								&ni_mm);
			//printf("search = %d, ni_mm = %p\n", err, ni_mm);
			ctlra_dev_disconnect(ni_mm);
		}
		return 0;
	}

	if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {

		libusb_device_handle *handle = 0;
		ret = libusb_open(dev, &handle);
		if (ret != LIBUSB_SUCCESS)
			return -1;
		uint8_t buf[255];
		ctlra_usb_impl_get_serial(handle, desc.iSerialNumber,
					  buf, 255);
#if 0
		printf("Device attached: %04x:%04x, serial %s, ctlra %p\n",
		       desc.idVendor, desc.idProduct, buf, user_data);
#endif
		/* Quirks:
		 * Here we can handle strange hotplug issues. For example,
		 * controllers that have a USB hub integrated show as the
		 * hub first (so the hotplug picks up that VID/PID pair,
		 * not the device itself for some reason). Here we can
		 * modify the VID/PID pair based on known corner cases:
		 */
		uint32_t quirk_vid = desc.idVendor;
		uint32_t quirk_pid = desc.idProduct;
		switch(quirk_vid) {
		case 0x17cc:
			/* NI Kontrol D2, change PID from 0x1403 (hub) back
			 * to the normal PID of 0x1400 */
			if(quirk_pid == 0x1403)
				quirk_pid = 0x1400;
			break;
		default: break;
		};

		int id = ctlra_impl_get_id_by_vid_pid(quirk_vid, quirk_pid);
		if(id < 0) {
			/* Device is not supported by Ctlra, so release
			 * the libusb handle which was opened to retrieve
			 * the serial from the device */
			libusb_close(handle);
			//printf("hotplugged device not supported by Ctlra\n");
			return -1;
		}

		int accepted = ctlra_impl_accept_dev(ctlra, id);
		if(!accepted)
			libusb_close(handle);
		return 0;
	}

	printf("%s: done & return 0\n", __func__);
	return 0;
}


void ctlra_impl_usb_idle_iter(struct ctlra_t *ctlra)
{
	struct timeval tv = {0};
	/* 1st: NULL context
	 * 2nd: timeval to wait - 0 returns as if non blocking
	 * 3rd: int* to completed event - unused by Ctlra */
	libusb_handle_events_timeout_completed(NULL, &tv, NULL);
}


int ctlra_dev_impl_usb_init(struct ctlra_t *ctlra)
{
	int ret;
	/* TODO: move this to a usb specific cltra_init() function */
	if(ctlra->usb_initialized)
		return -1;

	ret = libusb_init (&ctlra->ctx);
	if (ret < 0) {
		printf("failed to initialise libusb: %s\n",
		       libusb_error_name(ret));
		return -1;
	}
	ctlra->usb_initialized = 1;

	if(!libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG)) {
		printf ("Ctlra: No Hotplug on this platform\n");
		return -2;
	}

	/* setup hotplug callbacks */
	libusb_hotplug_callback_handle hp[2];
	ret = libusb_hotplug_register_callback(NULL,
					       /* Register arrive and leave */
					       LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
					       LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
					       0,
					       LIBUSB_HOTPLUG_MATCH_ANY,
					       LIBUSB_HOTPLUG_MATCH_ANY,
					       LIBUSB_HOTPLUG_MATCH_ANY,
					       ctlra_usb_impl_hotplug_cb,
					       ctlra,
					       &hp[0]);
	if (ret != LIBUSB_SUCCESS) {
		printf("hotplug register failure\n");
	}
	return 0;
}

int ctlra_dev_impl_usb_open(struct ctlra_dev_t *ctlra_dev, int vid,
                            int pid)
{
	int ret;

	libusb_device **devs;
	libusb_device *dev;
	int i = 0, j = 0;
	uint8_t path[USB_PATH_MAX];

	int cnt = libusb_get_device_list(NULL, &devs);
	if (cnt < 0)
		goto fail;

	while ((dev = devs[i++]) != NULL) {
		struct libusb_device_descriptor desc;
		int r = libusb_get_device_descriptor(dev, &desc);
		if (r < 0) {
			printf("failed to get device descriptor");
			goto fail;
		}
#if 0
		printf("%04x:%04x (serial %d) (bus %d, device %d)",
		       desc.idVendor, desc.idProduct, desc.iSerialNumber,
		       libusb_get_bus_number(dev),
		       libusb_get_device_address(dev));

		r = libusb_get_port_numbers(dev, path, sizeof(path));
		if (r > 0) {
			printf(" path: %d", path[0]);
			for (j = 1; j < r; j++)
				printf(".%d", path[j]);
		}
		printf("\n");
#endif

		if(desc.idVendor  == vid &&
		    desc.idProduct == pid) {
			ctlra_dev->info.serial_number = desc.iSerialNumber;
			ctlra_dev->info.vendor_id     = desc.idVendor;
			ctlra_dev->info.device_id     = desc.idProduct;
			break;
		}
	}

	libusb_free_device_list(devs, 1);

	if(!dev)
		goto fail;
	ctlra_dev->usb_device = dev;

	memset(ctlra_dev->usb_interface, 0,
	       sizeof(ctlra_dev->usb_interface));

	return 0;
fail:
	return -1;
}

int ctlra_dev_impl_usb_open_interface(struct ctlra_dev_t *ctlra_dev,
                                      int interface,
                                      int handle_idx)
{
	if(handle_idx >= CTLRA_USB_IFACE_PER_DEV) {
		printf("request for handle beyond available iface per dev range\n");
		return -1;
	}
	libusb_device *usb_dev = ctlra_dev->usb_device;
	libusb_device_handle *handle = 0;

	/* now that we've found the device, open the handle */
	int ret = libusb_open(usb_dev, &handle);
	if(ret != LIBUSB_SUCCESS) {
		printf("Error in opening interface, dev %s\n",
		       ctlra_dev->info.device);
		return -1;
	}

	ctlra_usb_impl_get_serial(handle, ctlra_dev->info.serial_number,
				  (uint8_t*)ctlra_dev->info.serial,
				  CTLRA_DEV_SERIAL_MAX);

	/* enable auto management of kernel claiming / unclaiming */
	if (libusb_has_capability(LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER)) {
		ret = libusb_set_auto_detach_kernel_driver(handle, 1);
		if(ret != LIBUSB_SUCCESS) {
			printf("Error setting auto kernel unclaiming\n");
			return -1;
		}
	} else {
		printf("Warning: auto kernel claim/unclaiming not supported\n");
	}

	ret = libusb_claim_interface(handle, interface);
	if(ret != LIBUSB_SUCCESS) {
		printf("Ctlra: Could not claim interface %d of dev %s,"
		       "continuing...\n", interface,
		       ctlra_dev->info.device);
		int kernel_active = libusb_kernel_driver_active(handle,
		                    interface);
		if(kernel_active)
			printf("=> Kernel has claimed the interface. Stop"
			       "other applications using this device and retry\n");
		return -1;
	}

	/* Commit to success: update handles in struct and return ok*/
	ctlra_dev->usb_interface[handle_idx] = handle;

	return 0;
}

/* Marks a device as failed, and adds it to the disconnect list. After
 * having been banished, the device instance will not function again */
void ctlra_dev_impl_banish(struct ctlra_dev_t *dev)
{
	struct ctlra_t *ctlra = dev->ctlra_context;
	dev->banished = 1;
	if(ctlra->banished_list == 0)
		ctlra->banished_list = dev;
	else {
		struct ctlra_dev_t *dev_iter = ctlra->banished_list;
		while(dev_iter->banished_list_next)
			dev_iter = dev_iter->banished_list_next;
		dev_iter->banished_list_next = dev;
		dev->banished_list_next = 0;
	}
}

int ctlra_dev_impl_usb_interrupt_read(struct ctlra_dev_t *dev, uint32_t idx,
                                      uint32_t endpoint, uint8_t *data,
                                      uint32_t size)
{
	int transferred;
	const uint32_t timeout = 10;
	int r = libusb_interrupt_transfer(dev->usb_interface[idx], endpoint,
	                                  data, size, &transferred, timeout);
	if(r == LIBUSB_ERROR_TIMEOUT)
		return 0;
	if (r < 0) {
		/*
		fprintf(stderr, "ctlra: usb error %s : %s\n",
			libusb_error_name(r), libusb_strerror(r));
		*/
		ctlra_dev_impl_banish(dev);
		return r;
	}
	return transferred;
}

int ctlra_dev_impl_usb_interrupt_write(struct ctlra_dev_t *dev, uint32_t idx,
                                       uint32_t endpoint, uint8_t *data,
                                       uint32_t size)
{
	int transferred;
	const uint32_t timeout = 100;
	int r = libusb_interrupt_transfer(dev->usb_interface[idx], endpoint,
	                                  data, size, &transferred, timeout);
	if(r == LIBUSB_ERROR_TIMEOUT)
		return 0;
	if (r < 0) {
		/*
		fprintf(stderr, "ctlra: usb error %s : %s\n",
			libusb_error_name(r), libusb_strerror(r));
		*/
		ctlra_dev_impl_banish(dev);
		return r;
	}
	return transferred;
}

int ctlra_dev_impl_usb_bulk_write(struct ctlra_dev_t *dev, uint32_t idx,
                                  uint32_t endpoint, uint8_t *data,
                                  uint32_t size)
{
	const uint32_t timeout = 100;
	int transferred;
	int r = libusb_bulk_transfer(dev->usb_interface[idx], endpoint,
	                               data, size, &transferred, timeout);
	if(r == LIBUSB_ERROR_TIMEOUT)
		return 0;
	if (r < 0) {
		/*
		fprintf(stderr, "ctlra: usb error %s : %s\n",
			libusb_error_name(r), libusb_strerror(r));
		*/
		ctlra_dev_impl_banish(dev);
		return r;
	}
	return transferred;

}

void ctlra_dev_impl_usb_close(struct ctlra_dev_t *dev)
{
	for(int i = 0; i < CTLRA_USB_IFACE_PER_DEV; i++) {
		if(dev->usb_interface[i]) {
#if 0
			// Running this always seems to throw an error,
			// and it has no negative side-effects to not?
			int ret = libusb_release_interface(dev->usb_device, i);
			if(ret == LIBUSB_ERROR_NOT_FOUND) {
				// Seems to always happen? LibUSB bug?
				//printf("%s: release interface error: interface %d not found, continuing...\n", __func__, i);
			} else if (ret == LIBUSB_ERROR_NO_DEVICE)
				printf("%s: release interface error: no device, continuing...\n", __func__);
			else if(ret < 0) {
				printf("%s:Ctrla Warning: release interface ret: %d\n", __func__, ret);
			}
#endif
			libusb_close(dev->usb_interface[i]);
		}
	}
}

void ctlra_impl_usb_shutdown(struct ctlra_t *ctlra)
{
	libusb_exit(ctlra->ctx);
}

