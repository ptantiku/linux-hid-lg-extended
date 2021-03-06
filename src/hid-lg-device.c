/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/hid.h>
#include <linux/hid-lg-extended.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define LG_DEVICE_BUFSIZE 32

struct lg_device_buf {
	u8 data[HID_MAX_BUFFER_SIZE];
	size_t size;
};

struct lg_device_queue {
	spinlock_t qlock;
	u8 head;
	u8 tail;
	struct lg_device_buf queue[LG_DEVICE_BUFSIZE];
	struct work_struct worker;

	struct lg_device *main_device;
};

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
EXPORT_SYMBOL_GPL(lg_device_queue);

static ssize_t lg_device_hid_send(struct hid_device *hdev, u8 *buffer,
								size_t count)
{
	u8 *buf;
	ssize_t ret;

	if (!hdev->ll_driver->raw_request || !hdev->ll_driver->output_report)
		return -ENODEV;

	buf = kmemdup(buffer, count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_output_report(hdev, buf, count);

	if (ret != -ENOSYS)
	    goto out_free;

	ret = hid_hw_raw_request(hdev, buf[0], buf, count, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);

out_free:
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

	if (!device) {
		hid_err(hdev, "Trying to handle an event on a not initialized device, aborting");
		return 0;
	}

	lg_device_queue(device, device->in_queue, raw_data, size);

	return 0;
}

int lg_device_init(struct lg_device *device,
					struct hid_device *hdev,
					struct lg_driver *driver)
{
	int ret;
	device->out_queue = kzalloc(sizeof(*device->out_queue), GFP_KERNEL);
	if (!device->out_queue) {
		ret = -ENOMEM;
		goto err_free_dev;
	}

	device->in_queue = kzalloc(sizeof(*device->in_queue), GFP_KERNEL);
	if (!device->in_queue) {
		ret = -ENOMEM;
		goto err_free_out;
	}

	device->hdev = hdev;
	device->driver = driver;
	hid_set_drvdata(hdev, device);

	device->out_queue->main_device = device;
	spin_lock_init(&device->out_queue->qlock);
	INIT_WORK(&device->out_queue->worker, lg_device_send_worker);

	device->in_queue->main_device = device;
	spin_lock_init(&device->in_queue->qlock);
	INIT_WORK(&device->in_queue->worker, lg_device_receive_worker);

	return 0;
err_free_out:
	kfree(device->out_queue);
err_free_dev:
	kfree(device);
	return ret;
}
EXPORT_SYMBOL_GPL(lg_device_init);

int lg_device_init_copy(struct lg_device *device,
					struct lg_device *from,
					struct lg_driver *driver)
{
	device->out_queue = from->out_queue;
	device->in_queue = from->in_queue;
	device->hdev = from->hdev;
	device->driver = driver;

	return 0;
}
EXPORT_SYMBOL_GPL(lg_device_init_copy);

void lg_device_destroy(struct lg_device *device)
{
	if (device->in_queue) {
		if (device != device->in_queue->main_device)
			return;
	} else if (device->out_queue) {
		if (device != device->out_queue->main_device)
			return;
	}
	if (device->in_queue && device->out_queue) {
		cancel_work_sync(&device->in_queue->worker);
		cancel_work_sync(&device->out_queue->worker);
	}

	if (device->in_queue)
		kfree(device->in_queue);
	if (device->out_queue)
		kfree(device->out_queue);

	hid_set_drvdata(device->hdev, NULL);
}
EXPORT_SYMBOL_GPL(lg_device_destroy);
