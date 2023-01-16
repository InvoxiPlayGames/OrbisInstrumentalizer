/*
    pad_hooks_ghl.c - OrbisInstrumentalizer
    Hooks to the libScePad library to detect USB HID devices.
    Designed for use with Guitar Hero Live.
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <stdint.h>
#include <stdbool.h>

#include <GoldHEN/Common.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
// uncomment when OpenOrbis merges #228
//#include <orbis/_types/pad.h>
#include "OrbisPadTypes.h"
// uncomment when OpenOrbis merges #229
//#include <orbis/Usbd.h>
#include "OrbisUsbd.h"
#include "xinput.h"

#define PLUGIN_NAME "OrbisInstrumentalizer"
#define final_printf(a, args...) klog("[" PLUGIN_NAME "] " a, ##args)

// can't link against Pad or use the regular Pad.h header i don't think
int (*scePadOpenExt)(int userID, int type, int index, OrbisPadExtParam *param);
int (*scePadRead)(int handle, OrbisPadData *data, int count);
int (*scePadReadState)(int handle, OrbisPadData *data);
int (*scePadGetControllerInformation)(int handle, OrbisPadInformation *info);
int (*scePadOutputReport)(int handle, int type, uint8_t *report, int length);

typedef enum _OIGHLDeviceType {
    GHL_Type_None,
    GHL_Type_HID,
    GHL_Type_XInput,
    //GHL_Type_XOne
} OIGHLDeviceType;

typedef struct _OIGHLOpenDevice {
    bool isOpen;
    int sceUserID;
    int scePadHandle;
    OIGHLDeviceType type;
    libusb_device_handle *usbDevice;
    uint8_t deviceAddress;
    uint8_t reportBuffer[30];
} OIGHLOpenDevice;

#define MAX_DEVICE_COUNT 4
static OIGHLOpenDevice open_devices[MAX_DEVICE_COUNT] = { 0 };

static OIGHLOpenDevice *OIGHLGetDeviceByUserID(int userID) {
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].sceUserID == userID) {
            return &open_devices[i];
        } else if (open_devices[i].isOpen == false) {
            open_devices[i].isOpen = true;
            open_devices[i].sceUserID = userID;
            return &open_devices[i];
        }
    }
    return NULL;
}

static OIGHLOpenDevice *OIGHLGetDeviceByHandle(int padHandle) {
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].isOpen == true && open_devices[i].scePadHandle == padHandle)
            return &open_devices[i];
    }
    return NULL;
}

static OIGHLOpenDevice *OIGHLGetDeviceByDeviceAddress(uint8_t deviceAddress) {
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].usbDevice != NULL && open_devices[i].deviceAddress == deviceAddress)
            return &open_devices[i];
    }
    return NULL;
}

static bool SearchForNewDevices(OIGHLOpenDevice *open_device) {
    libusb_device **list;
    libusb_device_handle *candidate = NULL;
    uint8_t cand_dev_addr = 0;
    OIGHLDeviceType cand_type = GHL_Type_None;
    int items = sceUsbdGetDeviceList(&list);
    for (int i = 0; i < items; i++) {
        cand_dev_addr = sceUsbdGetDeviceAddress(list[i]);
        if (OIGHLGetDeviceByDeviceAddress(cand_dev_addr) == NULL) {
            struct libusb_device_descriptor desc;
            sceUsbdGetDeviceDescriptor(list[i], &desc);
            if (desc.idVendor == 0x12BA && desc.idProduct == 0x074B) {
                cand_type = GHL_Type_HID;
                sceUsbdOpen(list[i], &candidate);
                final_printf("Opened PS3 GHL guitar at bus %02x!\n");
                break;
            } else if (desc.idVendor == 0x1430 && desc.idProduct == 0x070B) {
                cand_type = GHL_Type_XInput;
                sceUsbdOpen(list[i], &candidate);
                final_printf("Opened Xbox 360 GHL guitar at bus %02x!\n");
                break;
            }
        }
    }
    sceUsbdFreeDeviceList(list);
    if (candidate != NULL)
    {
        open_device->usbDevice = candidate;
        open_device->type = cand_type;
        open_device->deviceAddress = cand_dev_addr;
    }
    return candidate != NULL;
}

HOOK_INIT(scePadOpenExt);
int scePadOpenExt_hook(int userID, int type, int index, OrbisPadExtParam *param) {
    int r = HOOK_CONTINUE(scePadOpenExt, int(*)(int, int, int, OrbisPadExtParam *), userID, type, index, param);
    if (type == ORBIS_PAD_PORT_TYPE_SPECIAL && r >= 0) {
        OIGHLOpenDevice *device = OIGHLGetDeviceByUserID(userID);
        if (device != NULL) {
            device->scePadHandle = r;
        } else {
            final_printf("Failed to get device handle for user ID %i!\n", userID);
        }
    }
    return r;
}

static uint8_t usb_rec_buf[2][27] = { 0 };
static struct libusb_transfer *async;
static void libusb_callback(struct libusb_transfer *transfer) {
    if (transfer != NULL) {
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
            sceUsbdFillInterruptTransfer(transfer, transfer->dev_handle, 0x81, transfer->buffer, 30, libusb_callback, transfer->user_data, 0);
            sceUsbdSubmitTransfer(transfer);
        } else {
            final_printf("Transfer failed, disconnecting device!\n");
            OIGHLOpenDevice *device = (OIGHLOpenDevice *)transfer->user_data;
            if (device != NULL) {
                sceUsbdClose(device->usbDevice);
                device->usbDevice = NULL;
                device->type = GHL_Type_None;
            }
        }
    }
}

static void *scanThread(void *args) {
    while (1) {
        static int nothing[8] = { 0 }; // supposed to be a timeval or something
        sceUsbdHandleEventsTimeout(nothing);
        sceKernelUsleep(10);
    }
    scePthreadExit(NULL);
    return NULL;
}

HOOK_INIT(scePadGetControllerInformation);
int scePadGetControllerInformation_hook(int handle, OrbisPadInformation *info) {
    OIGHLOpenDevice *device = OIGHLGetDeviceByHandle(handle);
    int r = HOOK_CONTINUE(scePadGetControllerInformation, int(*)(int, OrbisPadInformation *), handle, info);
    if (device == NULL) // if this isn't a device we're responsible for, ignore it
        return r;
        
    if (info->connected == 0 && device->usbDevice == NULL) {
        if (SearchForNewDevices(device)) {
            final_printf("Found a device, starting transfers\n");
            async = sceUsbdAllocTransfer(1);
            async->buffer = device->reportBuffer;
            async->dev_handle = device->usbDevice;
            async->user_data = device;
            libusb_callback(async);
        }
    }

    if (device->usbDevice == NULL) // if it fails, give up
        return -1;

    info->connected = 1;
    info->count = 1;
    info->connectionType = ORBIS_PAD_CONNECTION_TYPE_STANDARD;
    info->deviceClass = ORBIS_PAD_DEVICE_CLASS_GUITAR;
    return 0;
}

static void ParseXInputToPadStruct(xinput_report_controls *report, OrbisPadData *pad) {
    pad->buttons = 0;

    // fret buttons
    pad->buttons |= ((report->buttons2 & 0x10) != 0x00) ? ORBIS_PAD_BUTTON_CROSS : 0; // b1
    pad->buttons |= ((report->buttons2 & 0x20) != 0x00) ? ORBIS_PAD_BUTTON_CIRCLE : 0; // b2
    pad->buttons |= ((report->buttons2 & 0x80) != 0x00) ? ORBIS_PAD_BUTTON_TRIANGLE : 0; // b3
    pad->buttons |= ((report->buttons2 & 0x40) != 0x00) ? ORBIS_PAD_BUTTON_SQUARE : 0; // w1
    pad->buttons |= ((report->buttons2 & 0x01) != 0x00) ? ORBIS_PAD_BUTTON_L1 : 0; // w2
    pad->buttons |= ((report->buttons2 & 0x02) != 0x00) ? ORBIS_PAD_BUTTON_R1 : 0; // w3

    // strum bar, due to enable packet being required lets just use the dpad and lie
    pad->leftStick.y = 0x80;
    if (report->left_stick_y == 32767 || (report->buttons1 & 0x02) != 0x00)
        pad->leftStick.y = 0xFF;
    if (report->left_stick_y == -32768 || (report->buttons1 & 0x01) != 0x00)
        pad->leftStick.y = 0x00;

    // dpad (nav + strumming)
    if ((report->buttons1 & 0x01) != 0x00)
        pad->buttons |= ORBIS_PAD_BUTTON_UP;
    if ((report->buttons1 & 0x04) != 0x00)
        pad->buttons |= ORBIS_PAD_BUTTON_LEFT;
    if ((report->buttons1 & 0x02) != 0x00)
        pad->buttons |= ORBIS_PAD_BUTTON_DOWN;
    if ((report->buttons1 & 0x08) != 0x00)
        pad->buttons |= ORBIS_PAD_BUTTON_RIGHT;

    // special buttons (start, ghtv)
    pad->buttons |= ((report->buttons1 & 0x20) != 0x00) ? ORBIS_PAD_BUTTON_R3 : 0; // hero power/select button
    pad->buttons |= ((report->buttons1 & 0x10) != 0x00) ? ORBIS_PAD_BUTTON_OPTIONS : 0; // pause/start button
    pad->buttons |= ((report->buttons1 & 0x40) != 0x00) ? ORBIS_PAD_BUTTON_L3 : 0; // GHTV button

    // whammy
    pad->rightStick.y = (uint8_t)(report->right_stick_y / 0x100);
    // tilt
    pad->rightStick.x = (uint8_t)(report->right_stick_x / 0x100);
}

static void ParseHIDToPadStruct(uint8_t *hid_report, OrbisPadData *pad) {
    uint8_t frets = hid_report[0];
    uint8_t buttons = hid_report[1];
    uint8_t dpad = hid_report[2];
    uint8_t strum = hid_report[4];
    uint8_t whammy = hid_report[6];
    uint8_t tilt = hid_report[19];
    pad->buttons = 0;

    // fret buttons
    pad->buttons |= ((frets & 0x02) != 0x00) ? ORBIS_PAD_BUTTON_CROSS : 0; // b1
    pad->buttons |= ((frets & 0x04) != 0x00) ? ORBIS_PAD_BUTTON_CIRCLE : 0; // b2
    pad->buttons |= ((frets & 0x08) != 0x00) ? ORBIS_PAD_BUTTON_TRIANGLE : 0; // b3
    pad->buttons |= ((frets & 0x01) != 0x00) ? ORBIS_PAD_BUTTON_SQUARE : 0; // w1
    pad->buttons |= ((frets & 0x10) != 0x00) ? ORBIS_PAD_BUTTON_L1 : 0; // w2
    pad->buttons |= ((frets & 0x20) != 0x00) ? ORBIS_PAD_BUTTON_R1 : 0; // w3

    // strum bar
    pad->leftStick.y = strum;

    // dpad (nav + strumming)
    if (dpad == 0x00)
        pad->buttons |= ORBIS_PAD_BUTTON_UP;
    if (dpad == 0x02)
        pad->buttons |= ORBIS_PAD_BUTTON_LEFT;
    if (dpad == 0x04)
        pad->buttons |= ORBIS_PAD_BUTTON_DOWN;
    if (dpad == 0x06)
        pad->buttons |= ORBIS_PAD_BUTTON_RIGHT;

    // special buttons (start, ghtv)
    pad->buttons |= ((buttons & 0x01) != 0x00) ? ORBIS_PAD_BUTTON_R3 : 0; // hero power/select button
    pad->buttons |= ((buttons & 0x02) != 0x00) ? ORBIS_PAD_BUTTON_OPTIONS : 0; // pause/start button
    pad->buttons |= ((buttons & 0x04) != 0x00) ? ORBIS_PAD_BUTTON_L3 : 0; // GHTV button

    pad->rightStick.y = whammy;
    pad->rightStick.x = tilt;
}

int retry_cnt = 0;
uint8_t count;
HOOK_INIT(scePadReadState);
int scePadReadState_hook(int handle, OrbisPadData *data) {
    OIGHLOpenDevice *device = OIGHLGetDeviceByHandle(handle);
    int r = scePadRead(handle, data, 1);
    if (device == NULL) // we arne't responsible for this at all, so ignore
        return 0;
    
    if (data->connected == 0) // if there's no controller attached to this already, 
        data->connected = 1; // set connected to true so it requests info anyway, we can go from there

    if (device->usbDevice == NULL)
        return r;

    data->count = count++;

    if (device->type == GHL_Type_HID)
        ParseHIDToPadStruct(device->reportBuffer, data);
    else if (device->type == GHL_Type_XInput) {
        if (device->reportBuffer[0] == 0x00)
            ParseXInputToPadStruct((xinput_report_controls *)device->reportBuffer, data);
    } else
        return r;
    return 0;
}


HOOK_INIT(scePadOutputReport);
int scePadOutputReport_hook(int handle, int type, uint8_t *report, int length) {
    OIGHLOpenDevice *device = OIGHLGetDeviceByHandle(handle);
    if (device == NULL || device->usbDevice == NULL) // if this isn't a device we're responsible for, ignore it
        return HOOK_CONTINUE(scePadOutputReport, int(*)(int, int, uint8_t *, int), handle, type, report, length);

    if (device->type == GHL_Type_HID) {
        // PS3/Wii U guitars require a constant keepalive packet to have strums work
        // TODO: set the LED index in this packet
        unsigned char keepalive[] = { 0x02, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        sceUsbdControlTransfer(device->usbDevice, 0x21, 0x09, 0x0201, 0x0000, keepalive, sizeof(keepalive), 6);
    }
    return 0;
}

void InitPadHooks() {
    // make sure we have the USBD module loaded into memory
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    sceUsbdInit();

    // apply all the hooks to the pad library
    int pad = 0;
    sys_dynlib_load_prx("libScePad.sprx", &pad);
    sys_dynlib_dlsym(pad, "scePadGetControllerInformation", &scePadGetControllerInformation);
    sys_dynlib_dlsym(pad, "scePadReadState", &scePadReadState);
    sys_dynlib_dlsym(pad, "scePadOpenExt", &scePadOpenExt);
    sys_dynlib_dlsym(pad, "scePadRead", &scePadRead);
    sys_dynlib_dlsym(pad, "scePadOutputReport", &scePadOutputReport);
    HOOK(scePadGetControllerInformation);
    HOOK(scePadReadState);
    HOOK(scePadOpenExt);
    HOOK(scePadOutputReport);
    
    // to try to be as fast as possible taking inputs, use async transfers and a thread
    OrbisPthread thread;
    scePthreadCreate(&thread, NULL, scanThread, NULL, "OrbisInstrumentGHLThread");
}

void DestroyPadHooks() {
    // unhook everything just in case
    UNHOOK(scePadGetControllerInformation);
    UNHOOK(scePadReadState);
    UNHOOK(scePadOpenExt);
    UNHOOK(scePadOutputReport);
}
