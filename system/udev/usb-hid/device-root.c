// Copyright 2016 The Fuchsia Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "device.h"
#include "usb-hid.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define to_hid_root(d) containerof(d, usb_hid_dev_root_t, dev)

typedef struct hid_item {
    uint8_t bSize;
    uint8_t bType;
    uint8_t bTag;
    int64_t data;
} hid_item_t;

static const uint8_t* hid_parse_short_item(const uint8_t* buf, const uint8_t* end, hid_item_t* item) {
    switch (*buf & 0x3) {
    case 0:
        item->bSize = 0;
        break;
    case 1:
        item->bSize = 1;
        break;
    case 2:
        item->bSize = 2;
        break;
    case 3:
        item->bSize = 4;
        break;
    }
    item->bType = (*buf >> 2) & 0x3;
    item->bTag = (*buf >> 4) & 0x0f;
    if (buf + item->bSize >= end) {
        // Return a RESERVED item type, and point past the end of the buffer to
        // prevent further parsing.
        item->bType = 0x03;
        return end;
    }
    buf++;

    item->data = 0;
    for (uint8_t i = 0; i < item->bSize; i++) {
        item->data |= *buf << (8*i);
        buf++;
    }
    return buf;
}

static void hid_init_report_sizes(usb_hid_dev_root_t* hid) {
    for (int i = 0; i < HID_MAX_REPORT_IDS; i++) {
        hid->sizes[i].id = -1;
    }
}

static int hid_find_report_id(input_report_id_t report_id, usb_hid_dev_root_t* hid) {
    for (int i = 0; i < HID_MAX_REPORT_IDS; i++) {
        if (hid->sizes[i].id == report_id) return i;
        if (hid->sizes[i].id == -1) {
            hid->sizes[i].id = report_id;
            hid->num_reports++;
            return i;
        }
    }
    return -1;
}

void hid_read_report_sizes(usb_hid_dev_root_t* hid, const uint8_t* buf, size_t len) {
    const uint8_t* end = buf + len;
    hid_item_t item;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    input_report_id_t report_id = 0;
    while (buf < end) {
        buf = hid_parse_short_item(buf, end, &item);
        switch (item.bType) {
        case 0: {
            input_report_size_t inc = report_size * report_count;
            int idx;
            switch (item.bTag) {
            case 8:
                idx = hid_find_report_id(report_id, hid);
                assert(idx >= 0);
                hid->sizes[idx].in_size += inc;
                break;
            case 9:
                idx = hid_find_report_id(report_id, hid);
                assert(idx >= 0);
                hid->sizes[idx].out_size += inc;
                break;
            case 11:
                idx = hid_find_report_id(report_id, hid);
                assert(idx >= 0);
                hid->sizes[idx].feat_size += inc;
                break;
            default:
                break;
            }
            break;
        }
        case 1: {
            switch (item.bTag) {
            case 7:
                report_size = (uint32_t)item.data;
                break;
            case 8:
                report_id = (input_report_id_t)item.data;
                break;
            case 9:
                report_count = (uint32_t)item.data;
                break;
            case 10:
            case 11:
                printf("push/pop not supported!\n");
                assert(0);
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
    }
#ifdef USB_HID_DEBUG
    printf("num reports: %lu\n", hid->num_reports);
    for (size_t i = 0; i < hid->num_reports; i++) {
        if (hid->sizes[i].id >= 0) {
            printf("report id: %u  report sizes: in %u out %u feat %u\n",
                    hid->sizes[i].id, hid->sizes[i].in_size, hid->sizes[i].out_size,
                    hid->sizes[i].feat_size);
        }
    }
#endif
}

mx_status_t usb_hid_create_root(usb_hid_dev_root_t** hid) {
    *hid = calloc(1, sizeof(usb_hid_dev_root_t));
    if (*hid == NULL) {
        return ERR_NO_MEMORY;
    }
    hid_init_report_sizes(*hid);
    (*hid)->instance_lock = MXR_MUTEX_INIT;
    list_initialize(&((*hid)->instance_list));
    return NO_ERROR;
}

void usb_hid_cleanup_root(usb_hid_dev_root_t* hid) {
    if (hid->req) {
        hid->usb->free_request(hid->usbdev, hid->req);
    }
    if (hid->hid_report_desc) {
        free(hid->hid_report_desc);
    }
    free(hid);
    // TODO: cleanup instances? or maybe the root won't be released until all
    // instances are...
}

static mx_status_t usb_hid_open_root(mx_device_t* dev, mx_device_t** dev_out, uint32_t flags) {
    usb_hid_dev_root_t* hid = to_hid_root(dev);

    usb_hid_dev_instance_t* inst = NULL;
    mx_status_t status = usb_hid_create_instance(&inst);
    if (inst == NULL) {
        return ERR_NO_MEMORY;
    }

    status = device_init(&inst->dev, hid->drv, "usb-hid", &usb_hid_instance_proto);
    if (status != NO_ERROR) {
        usb_hid_cleanup_instance(inst);
        return status;
    }

    inst->dev.protocol_id = MX_PROTOCOL_INPUT;
    status = device_add_instance(&inst->dev, dev);
    if (status != NO_ERROR) {
        usb_hid_cleanup_instance(inst);
        return status;
    }
    inst->root = hid;

    mxr_mutex_lock(&hid->instance_lock);
    list_add_tail(&hid->instance_list, &inst->node);
    mxr_mutex_unlock(&hid->instance_lock);

    *dev_out = &inst->dev;
    return NO_ERROR;
}

static mx_status_t usb_hid_release_root(mx_device_t* dev) {
    usb_hid_dev_root_t* hid = to_hid_root(dev);
    usb_hid_cleanup_root(hid);
    return NO_ERROR;
}

mx_protocol_device_t usb_hid_root_proto = {
    .open = usb_hid_open_root,
    .release = usb_hid_release_root,
};