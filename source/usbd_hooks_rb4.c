/*
    usbd_hooks_rb4.c - OrbisInstrumentalizer
    Hooks to the libSceUsbd library to detect XInput instruments.
    Designed for use with Rock Band 4.
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <stdint.h>
#include <stdbool.h>

#include <GoldHEN/Common.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
// uncomment when OpenOrbis merges #229
//#include <orbis/Usbd.h>
#include "OrbisUsbd.h"
#include "xinput.h"

#define PLUGIN_NAME "OrbisInstrumentalizer"
#define final_printf(a, args...) klog("[" PLUGIN_NAME "] " a, ##args)

typedef enum _OIRB4DeviceType {
    RB4_Type_None,
    RB4_Type_XInputWireless,
    RB4_Type_XInputGuitar,
    RB4_Type_XInputDrums
} OIRB4DeviceType;

typedef struct _OIRB4OpenDevice {
    bool is_open;
    libusb_device *device;
    libusb_device_handle *device_handle;
    OIRB4DeviceType type;
    uint8_t last_report[30];
} OIRB4OpenDevice;

#define BIT(i) (1 << i)
#define MAX_DEVICE_COUNT 4
static OIRB4OpenDevice open_devices[MAX_DEVICE_COUNT] = { 0 };
static libusb_device_handle *x360_leds[MAX_DEVICE_COUNT] = { NULL };

int(*TsceUsbdOpen)(int);
int(*TsceUsbdClose)(int);
int(*TsceUsbdGetConfigDescriptor)(int);
int(*TsceUsbdGetDeviceDescriptor)(int);
int(*TsceUsbdFillInterruptTransfer)(int);

HOOK_INIT(TsceUsbdOpen);
HOOK_INIT(TsceUsbdClose);
HOOK_INIT(TsceUsbdGetConfigDescriptor);
HOOK_INIT(TsceUsbdGetDeviceDescriptor);
HOOK_INIT(TsceUsbdFillInterruptTransfer);

static OIRB4OpenDevice *GetOpenDeviceFromDevice(libusb_device *device) {
    //final_printf("GetOpenDeviceFromDevice\n");
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].device == device) {
            return &open_devices[i];
        } else if (open_devices[i].is_open == false) {
            open_devices[i].is_open = true;
            open_devices[i].device = device;
            return &open_devices[i];
        }
    }
    return NULL;
}

static OIRB4OpenDevice *GetOpenDeviceFromDeviceHandle(libusb_device_handle *device_handle) {
    //final_printf("GetOpenDeviceFromDeviceHandle\n");
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].is_open && open_devices[i].device_handle == device_handle)
            return &open_devices[i];
    }
    return NULL;
}

static OIRB4DeviceType IdentifyDevice(libusb_device *device) {
    //final_printf("IdentifyDevice\n");
    struct libusb_device_descriptor *desc;
    struct libusb_config_descriptor *config;
    int r = 0;
    int type = RB4_Type_None;
    r = sceUsbdGetConfigDescriptor(device, 0, &config);
    if (r == 0) {
        // the xinput devices have a class of 0xFF and subclass of 0x5D
        // just in case whatever we messed with breaks, use subclass rather than class
        if (config->interface[0].altsetting->bInterfaceSubClass == 0x5D) {
            if (config->interface[0].altsetting->extra != NULL && config->interface[0].altsetting->extra_length > 6) {
                // controller subtype is at the 5th byte
                uint8_t subtype = config->interface[0].altsetting->extra[4];
                if (subtype == XINPUT_SUBTYPE_GUITAR ||
                    subtype == XINPUT_SUBTYPE_GUITAR_ALTERNATE ||
                    subtype == XINPUT_SUBTYPE_GUITAR_BASS) {
                    type = RB4_Type_XInputGuitar;
                } else if (subtype == XINPUT_SUBTYPE_DRUM_KIT ||
                           subtype == XINPUT_SUBTYPE_GAMEPAD) { // (for testing purposes)
                    type = RB4_Type_XInputDrums;
                } else {
                    // if we're not a known subtype, assume that we're a wireless dongle (mine has 0x13 at this position)
                    type = RB4_Type_XInputWireless;
                }
            }
        }
    }
    sceUsbdFreeConfigDescriptor(config);
    return type;
}

// TODO: this seems bad i dont even know if it do drum
typedef struct _ps3_rb_guitar_report {
    uint16_t buttons;
    uint8_t hat;
    uint8_t left_joy_x;
    uint8_t left_joy_y;
    uint8_t whammy;
    uint8_t mode_switch;
    uint8_t padding[12];
    uint8_t accel_x;
    uint8_t unk_1;
    uint16_t accel_z;
    uint8_t accel_y;
    uint8_t unk_2;
    uint16_t gyro;
} ps3_rb_guitar_report;

libusb_transfer_cb_fn interrupt_callback;

void ParseXInputCallback(struct libusb_transfer *transfer) {
    //final_printf("ParseXInputCallback\n");
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        ps3_rb_guitar_report parsed_report = { 0 };
        xinput_report_controls *xparsed = (xinput_report_controls *)transfer->buffer;

        // Lookup device type so we know how to handle inputs
        OIRB4DeviceType instrument_type = RB4_Type_None;
        libusb_device_handle *dev_handle = transfer->dev_handle;
        for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
            if (open_devices[i].is_open && open_devices[i].device_handle == dev_handle) {
                instrument_type = open_devices[i].type;
                break;
            }
        }

        // Common buttons/frets/pads
        if ((xparsed->buttons2 & XINPUT_BUTTON_A) != 0) parsed_report.buttons |= BIT(1);     // Green
        if ((xparsed->buttons2 & XINPUT_BUTTON_B) != 0) parsed_report.buttons |= BIT(2);     // Red
        if ((xparsed->buttons2 & XINPUT_BUTTON_Y) != 0) parsed_report.buttons |= BIT(3);     // Yellow
        if ((xparsed->buttons2 & XINPUT_BUTTON_X) != 0) parsed_report.buttons |= BIT(0);     // Blue
        if ((xparsed->buttons1 & XINPUT_BUTTON_BACK) != 0) parsed_report.buttons |= BIT(8);  // Back
        if ((xparsed->buttons1 & XINPUT_BUTTON_START) != 0) parsed_report.buttons |= BIT(9); // Start

        // Common dpad/strum
        parsed_report.hat = 0x08;  // Center
        if ((xparsed->buttons1 & XINPUT_BUTTON_UP) != 0) parsed_report.hat = 0x00;
        if ((xparsed->buttons1 & XINPUT_BUTTON_DOWN) != 0) parsed_report.hat = 0x04;
        if ((xparsed->buttons1 & XINPUT_BUTTON_LEFT) != 0) parsed_report.hat = 0x06;
        if ((xparsed->buttons1 & XINPUT_BUTTON_RIGHT) != 0) parsed_report.hat = 0x02;

        if (instrument_type == RB4_Type_XInputGuitar) {
            if ((xparsed->buttons2 & XINPUT_BUTTON_LB) != 0) {parsed_report.buttons |= BIT(4);}   // Orange
            if ((xparsed->buttons1 & XINPUT_BUTTON_L3) != 0) {parsed_report.buttons |= BIT(6);}   // Lower frets/solo modifer
            if (xparsed->right_stick_y >= 0x4000) {parsed_report.buttons |= BIT(5);}              // Tilt (guitar sends value between 0 and 0x7FFF, boil down to a button press)
            parsed_report.whammy = (uint8_t)((uint16_t)xparsed->right_stick_x / 0x100);           // Whammy (analog)

        } else if (instrument_type == RB4_Type_XInputDrums) {
            if ((xparsed->buttons2 & XINPUT_BUTTON_LB) != 0) {parsed_report.buttons |= BIT(4);}   // Kick pedal 1
            if ((xparsed->buttons1 & XINPUT_BUTTON_L3) != 0) {parsed_report.buttons |= BIT(5);}   // Kick pedal 2
            if ((xparsed->buttons1 & XINPUT_BUTTON_R3) != 0) {parsed_report.buttons |= BIT(10);}  // Pad modifier
            if ((xparsed->buttons2 & XINPUT_BUTTON_RB) != 0) {parsed_report.buttons |= BIT(11);}  // Cymbal modifier
        }

        // copy the new parsed report back into the buffer
        memcpy(transfer->buffer, &parsed_report, transfer->length);
    }
    interrupt_callback(transfer);
}

void ParseWirelessXInputCallback(struct libusb_transfer *transfer) {
    // sometimes the wireless report will just be a silly nothingpacket
    // so we have to log the last actual packet somewhere
    // muh memory latencyerinos
    //final_printf("ParseWirelessXInputCallback\n");
    OIRB4OpenDevice *device = GetOpenDeviceFromDeviceHandle(transfer->dev_handle);
    if (device != NULL) {
        if (transfer->buffer[1] == 0x01) // new input data
            memcpy(device->last_report, transfer->buffer, transfer->length);
        // have this copy double as a way to fast-forward the input data by 4 bytes
        memcpy(transfer->buffer, device->last_report + 4, transfer->length - 4);
    }
    ParseXInputCallback(transfer);
}

void set_xbox_360_player_led(libusb_device_handle *handle, int playerIndex) {
    // Determine LED pattern
    uint8_t ledPattern = X360_LED_ALL_OFF;
    switch (playerIndex) {
        case 1: ledPattern = X360_LED_P1; break;
        case 2: ledPattern = X360_LED_P2; break;
        case 3: ledPattern = X360_LED_P3; break;
        case 4: ledPattern = X360_LED_P4; break;
        default: ledPattern = X360_LED_ALL_OFF; break;
    }

    uint8_t buf[] = {
        0x01,        // Report ID for LEDs
        0x03,        // Command: Set LED
        ledPattern   // LED pattern for player index
    };

    int res = sceUsbdControlTransfer(
        handle,
        0x21,                            // bmRequestType: Class | Interface | Host to Device
        0x09,                            // bRequest: SET_REPORT
        (2 << 8) | buf[0],               // wValue: (Output report << 8) | Report ID
        0,                               // wIndex: Interface number (0 assumed)
        buf,
        sizeof(buf),
        10                               // Timeout in ms
    );

    if (res < 0) {
        final_printf("LED control failed: %d\n", res);
    } else {
        final_printf("Set LED to player %d (pattern 0x%02X)\n", playerIndex, ledPattern);
    }
}

int TsceUsbdOpen_hook(libusb_device *device, libusb_device_handle **dev_handle) {
    //final_printf("sceUsbdOpen_hook\n");
    int r = sceUsbdOpen(device, dev_handle);
    if (r == 0 && dev_handle != NULL && *dev_handle != NULL) {
        OIRB4DeviceType type = IdentifyDevice(device);
        if (type != RB4_Type_None) {
            OIRB4OpenDevice *opendevice = GetOpenDeviceFromDevice(device);
            if (opendevice == NULL)
                return r;
            opendevice->type = type;
            opendevice->device_handle = *dev_handle;

            // Set XBOX 360 player LEDs
            if (type == RB4_Type_XInputGuitar || type == RB4_Type_XInputDrums) {
                // Find lowest available player slot
                int player_slot = -1;
                for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
                    if (x360_leds[i] == NULL) {
                        x360_leds[i] = *dev_handle;
                        player_slot = i+1;
                        break;
                    }
                }

                if (player_slot > 0) {
                    set_xbox_360_player_led(*dev_handle, player_slot);
                    final_printf("Assigned player slot %d\n", player_slot);
                } else {
                    final_printf("No free player slots available\n");
                }
                
            }
        }
    }
    return r;
}

void TsceUsbdClose_hook(libusb_device_handle *dev_handle) {
    //final_printf("sceUsbdClose_hook\n");
    if (dev_handle == NULL)
        return;

    // Release player slot
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (x360_leds[i] == dev_handle) {
            x360_leds[i] = NULL;
            final_printf("Released player slot %d\n", i + 1);
            break;
        }
    }

    sceUsbdClose(dev_handle);
    if (dev_handle != NULL) {
        OIRB4OpenDevice *opendevice = GetOpenDeviceFromDeviceHandle(dev_handle);
        if (opendevice == NULL)
            return;
        opendevice->is_open = false;
        opendevice->device_handle = NULL;
        opendevice->device = NULL;
    }
    return;
}

int TsceUsbdGetConfigDescriptor_hook(libusb_device *device, uint8_t config_index, struct libusb_config_descriptor **config) {
    //final_printf("sceUsbdGetConfigDescriptor_hook\n");
    int r = sceUsbdGetConfigDescriptor(device, config_index, config);
    // always set device class to HID - rb4 needs this to actually connect to the device
    if (r == 0 && config != NULL && *config != NULL)
        (*config)->interface->altsetting[0].bInterfaceClass = 0x03;
    return r;
}

int TsceUsbdGetDeviceDescriptor_hook(libusb_device *device, struct libusb_device_descriptor *desc) {
    //final_printf("sceUsbdGetDeviceDescriptor_hook\n");
    int r = sceUsbdGetDeviceDescriptor(device, desc);
    if (r == 0 && desc != NULL) {
        // Wii instruments use Harmonix Music Systems vendor id
        if (desc->idVendor == 0x1BAD) {
            desc->idVendor = 0x12BA; // licensed by SCEA
            switch(desc->idProduct) {
                // TODO: Fill more of these in, because there is more, and we must support them.
                case 0x0004:
                    // guitar
                    desc->idProduct = 0x0200;
                    break;
                case 0x0005:
                case 0x3110:
                case 0x3138:
                    // drums
                    desc->idProduct = 0x0210;
                    break;
            }
        }

        // detect XInput instruments using device class + interface descriptor
        // so third party ones should work just fine
        if (desc->bDeviceClass == 0xFF && desc->bDeviceSubClass == 0xFF && desc->bDeviceProtocol == 0xFF) {
            // get the type of controller
            switch (IdentifyDevice(device)) {
                case RB4_Type_XInputGuitar:
                    desc->idVendor = 0x12BA;
                    desc->idProduct = 0x0200;
                    break;
                case RB4_Type_XInputDrums:
                case RB4_Type_XInputWireless:
                    desc->idVendor = 0x12BA;
                    desc->idProduct = 0x0210;
                    break;
                default:
                    break;
            }
        }
        //final_printf("Descriptor: %04X %04X\n", desc->idVendor, desc->idProduct);
    }
    return r;
}

void TsceUsbdFillInterruptTransfer_hook(struct libusb_transfer *transfer, libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *buffer, int length, libusb_transfer_cb_fn callback, void *user_data, unsigned int timeout) {
    //final_printf("sceUsbdFillInterruptTransfer_hook\n");
    OIRB4OpenDevice *device = GetOpenDeviceFromDeviceHandle(dev_handle);
    if (device != NULL) {
        interrupt_callback = callback; // should always be the same
        if (device->type == RB4_Type_XInputWireless)
            callback = ParseWirelessXInputCallback;
        else
            callback = ParseXInputCallback;
    }
    sceUsbdFillInterruptTransfer(transfer, dev_handle, endpoint, buffer, length, callback, user_data, timeout);
}

#define ADDR_OFFSET 0x00400000
void InitUsbdHooks() {
    // make sure we have the USBD module loaded into memory
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    struct proc_info procInfo;
    sys_sdk_proc_info(&procInfo);

    if (strcmp(procInfo.version, "02.21") != 0) {
        final_printf("This plugin is only compatible with version 02.21 of Rock Band 4.\n");
        return;
    }

    /*int usbd = 0;
    sys_dynlib_load_prx("libSceUsbd.sprx", &usbd);
    sys_dynlib_dlsym(usbd, "sceUsbdGetConfigDescriptor", &TsceUsbdGetConfigDescriptor);
    sys_dynlib_dlsym(usbd, "sceUsbdFreeConfigDescriptor", &TsceUsbdFreeConfigDescriptor);
    sys_dynlib_dlsym(usbd, "sceUsbdGetDeviceDescriptor", &TsceUsbdGetDeviceDescriptor);
    sys_dynlib_dlsym(usbd, "sceUsbdFillInterruptTransfer", &TsceUsbdFillInterruptTransfer);
    sys_dynlib_dlsym(usbd, "sceUsbdOpen", &TsceUsbdOpen);
    sys_dynlib_dlsym(usbd, "sceUsbdClose", &TsceUsbdClose);*/

    // TODO: Figure out why we can't just hook sceUsbd directly
    // right now this is pretty bad form, to hook the game's own stubs
    // less game-agnostic (not that any other games use Usbd) and less version-agnostic
    TsceUsbdOpen = (void*)(procInfo.base_address + 0x01245260);
    TsceUsbdGetDeviceDescriptor = (void*)(procInfo.base_address + 0x01245210);
    TsceUsbdGetConfigDescriptor = (void*)(procInfo.base_address + 0x012451e0);
    TsceUsbdFillInterruptTransfer = (void*)(procInfo.base_address + 0x01245190);
    TsceUsbdClose = (void*)(procInfo.base_address + 0x01245160);

    // apply all the hooks to the usbd library
    HOOK(TsceUsbdGetConfigDescriptor);
    HOOK(TsceUsbdGetDeviceDescriptor);
    HOOK(TsceUsbdFillInterruptTransfer);
    HOOK(TsceUsbdOpen);
    HOOK(TsceUsbdClose);
}

void DestroyUsbdHooks() {
    // unhook everything just in case
    UNHOOK(TsceUsbdGetConfigDescriptor);
    UNHOOK(TsceUsbdGetDeviceDescriptor);
    UNHOOK(TsceUsbdFillInterruptTransfer);
    UNHOOK(TsceUsbdOpen);
    UNHOOK(TsceUsbdClose);
}