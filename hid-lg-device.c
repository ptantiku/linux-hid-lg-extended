#include <linux/hid.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "hid-lg-mx-revolution.h"
#include "hid-lg-core.h"
#include "hid-lg-device.h"
#include "hid-lg-mx5500-receiver.h"
#include "hid-lg-mx5500-keyboard.h"

void lg_device_set_data(struct lg_device *device, void *data)
{
	device->data = data;
}

void *lg_device_get_data(struct lg_device *device)
{
	return device->data;
}

struct lg_mx5500_receiver *lg_device_get_receiver(struct lg_device *device)
{
	struct lg_mx5500_receiver *receiver = NULL;
	if (device->driver->type == LG_MX5500_RECEIVER)
		receiver = lg_device_get_data(device);

	return receiver;
}

struct lg_mx5500_keyboard *lg_device_get_keyboard(struct lg_device *device)
{
	struct lg_mx5500_keyboard *keyboard = NULL;
	if (device->driver->type == LG_MX5500_KEYBOARD)
		keyboard = lg_device_get_data(device);
	else if (device->driver->type == LG_MX5500_RECEIVER)
		keyboard = ((struct lg_mx5500_receiver*)lg_device_get_receiver(device))->keyboard;

	return keyboard;
}

struct lg_mx_revolution *lg_device_get_mouse(struct lg_device *device)
{
	struct lg_mx_revolution *mouse = NULL;
	if (device->driver->type == LG_MX5500_MOUSE)
		mouse = lg_device_get_data(device);
	else if (device->driver->type == LG_MX5500_RECEIVER)
		mouse = ((struct lg_mx5500_receiver*)lg_device_get_receiver(device))->mouse;

	return mouse;
}

void lg_device_queue(struct lg_device *device, struct lg_device_queue *queue, const u8 *buffer,
								size_t count)
{
	unsigned long flags;
	u8 newhead;

	if (count > HID_MAX_BUFFER_SIZE) {
		hid_warn(device->hdev, "Sending too large output report\n");
		return;
	}

	spin_lock_irqsave(&queue->qlock, flags);

	memcpy(queue->queue[queue->head].data, buffer, count);
	queue->queue[queue->head].size = count;
	newhead = (queue->head + 1) % LG_DEVICE_BUFSIZE;

	if (queue->head == queue->tail) {
		queue->head = newhead;
		schedule_work(&queue->worker);
	} else if (newhead != queue->tail) {
		queue->head = newhead;
	} else {
		hid_warn(device->hdev, "Queue is full");
	}

	spin_unlock_irqrestore(&queue->qlock, flags);
}

static ssize_t lg_device_hid_send(struct hid_device *hdev, u8 *buffer,
								size_t count)
{
	u8 *buf;
	ssize_t ret;

	if (!hdev->hid_output_raw_report)
		return -ENODEV;

	buf = kmemdup(buffer, count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hdev->hid_output_raw_report(hdev, buf, count, HID_OUTPUT_REPORT);

	kfree(buf);
	return ret;
}

void lg_device_send_worker(struct work_struct *work)
{
	struct lg_device_queue *queue = container_of(work, struct lg_device_queue,
								worker);
	struct lg_device *device= queue->main_device;
	unsigned long flags;

	spin_lock_irqsave(&queue->qlock, flags);

	while (queue->head != queue->tail) {
		spin_unlock_irqrestore(&queue->qlock, flags);
		lg_device_hid_send(device->hdev, queue->queue[queue->tail].data,
						queue->queue[queue->tail].size);
		spin_lock_irqsave(&queue->qlock, flags);

		queue->tail = (queue->tail + 1) % LG_DEVICE_BUFSIZE;
	}

	spin_unlock_irqrestore(&queue->qlock, flags);
}

void lg_device_receive_worker(struct work_struct *work)
{
	struct lg_device_queue *queue = container_of(work, struct lg_device_queue,
								worker);
	struct lg_device *device= queue->main_device;
	unsigned long flags;

	spin_lock_irqsave(&queue->qlock, flags);

	while (queue->head != queue->tail) {
		spin_unlock_irqrestore(&queue->qlock, flags);
		if (device->driver->receive_handler)
			device->driver->receive_handler(device, queue->queue[queue->tail].data,
						queue->queue[queue->tail].size);
		spin_lock_irqsave(&queue->qlock, flags);

		queue->tail = (queue->tail + 1) % LG_DEVICE_BUFSIZE;
	}

	spin_unlock_irqrestore(&queue->qlock, flags);
}

int lg_device_event(struct hid_device *hdev, struct hid_report *report,
				u8 *raw_data, int size)
{
	struct lg_device *device;

	if (report->id < 0x10)
		return 0;

	device = hid_get_drvdata(hdev);

	lg_device_queue(device, device->in_queue, raw_data, size);

	return 0;
}

struct lg_device *lg_device_create(struct hid_device *hdev,
					struct lg_driver *driver)
{
	struct lg_device *device;

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		goto err;

	device->out_queue = kzalloc(sizeof(*device->out_queue), GFP_KERNEL);
	if (!device->out_queue)
		goto err_free_dev;

	device->in_queue = kzalloc(sizeof(*device->in_queue), GFP_KERNEL);
	if (!device->in_queue)
		goto err_free_out;

	device->hdev = hdev;
	device->data = NULL;
	device->driver = driver;
	hid_set_drvdata(hdev, device);

	device->out_queue->main_device = device;
	spin_lock_init(&device->out_queue->qlock);
	INIT_WORK(&device->out_queue->worker, lg_device_send_worker);

	device->in_queue->main_device = device;
	spin_lock_init(&device->in_queue->qlock);
	INIT_WORK(&device->in_queue->worker, lg_device_receive_worker);

	return device;
err_free_out:
	kfree(device->out_queue);
err_free_dev:
	kfree(device);
err:
	return NULL;
}

void lg_device_destroy(struct lg_device *device)
{
	cancel_work_sync(&device->in_queue->worker);
	cancel_work_sync(&device->out_queue->worker);
	kfree(device->in_queue);
	kfree(device->out_queue);
	kfree(device);
}
