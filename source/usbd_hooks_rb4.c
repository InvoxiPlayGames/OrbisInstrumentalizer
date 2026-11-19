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

void DoNotification(const char* text);

typedef enum _OIRB4DeviceType {
    RB4_Type_None,
    RB4_Type_XInputWireless,
    RB4_Type_XInputGuitar,
    RB4_Type_XInputDrums
} OIRB4DeviceType;

// what the game has been told a device is, which decides the report format we send it
typedef enum _OIRB4InstrumentKind {
    RB4_Kind_Guitar,
    RB4_Kind_Drums
} OIRB4InstrumentKind;

// what a wireless receiver is reported as when we couldn't find out what's linked to it
#define RB4_WIRELESS_DEFAULT_KIND RB4_Kind_Drums

#define PS3_REPORT_SIZE 27
#define XINPUT_CONTROLS_SIZE ((int)sizeof(xinput_report_controls))
#define XINPUT_OUT_BUFFER_SIZE 12

typedef struct _OIRB4OpenDevice {
    bool is_open;
    libusb_device *device;
    libusb_device_handle *device_handle;
    OIRB4DeviceType type;
    OIRB4InstrumentKind kind;
    uint8_t subtype; // XInput subtype of the instrument, 0 if not known yet
    // the last controls received, so non-input packets don't release everything
    bool has_input;
    uint8_t last_controls[sizeof(xinput_report_controls)];
    // player LED, sent over the interrupt OUT endpoint
    uint8_t out_endpoint;
    bool out_pending;
    bool led_set;
    uint8_t out_buffer[XINPUT_OUT_BUFFER_SIZE];
    bool mismatch_notified;
} OIRB4OpenDevice;

#define MAX_DEVICE_COUNT 4
static OIRB4OpenDevice open_devices[MAX_DEVICE_COUNT] = { 0 };

// Remembers what each wireless receiver was reported to the game as, so that asking
// the receiver what's linked to it only blocks the game once per receiver.
typedef struct _OIRB4WirelessReceiver {
    libusb_device *device;
    uint8_t bus;
    uint8_t address; // changes when replugged, in case the device pointer gets reused
    OIRB4InstrumentKind kind;
    uint8_t subtype;
} OIRB4WirelessReceiver;

#define MAX_WIRELESS_RECEIVERS 8
static OIRB4WirelessReceiver wireless_receivers[MAX_WIRELESS_RECEIVERS] = { 0 };
static int next_wireless_receiver = 0;

// Subtype of an instrument that linked to a receiver the game was told the wrong thing
// about. The receiver usually gets enumerated again before the instrument relinks to it
// when it's replugged, so the next receiver that shows up with nothing linked gets this.
static uint8_t pending_wireless_subtype = 0;

// the wireless receiver always uses these endpoints for the first controller slot
#define XINPUT_WIRELESS_IN_ENDPOINT  0x81
#define XINPUT_WIRELESS_OUT_ENDPOINT 0x01

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
    OIRB4OpenDevice *free_slot = NULL;
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].is_open && open_devices[i].device == device)
            return &open_devices[i];
        if (!open_devices[i].is_open && free_slot == NULL)
            free_slot = &open_devices[i];
    }
    if (free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->is_open = true;
        free_slot->device = device;
    }
    return free_slot;
}

static OIRB4OpenDevice *GetOpenDeviceFromDeviceHandle(libusb_device_handle *device_handle) {
    //final_printf("GetOpenDeviceFromDeviceHandle\n");
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].is_open && open_devices[i].device_handle == device_handle)
            return &open_devices[i];
    }
    return NULL;
}

static OIRB4DeviceType IdentifyDevice(libusb_device *device, uint8_t *subtype_out) {
    //final_printf("IdentifyDevice\n");
    struct libusb_config_descriptor *config = NULL;
    int r = 0;
    OIRB4DeviceType type = RB4_Type_None;
    uint8_t subtype = 0;
    r = sceUsbdGetConfigDescriptor(device, 0, &config);
    if (r == 0 && config != NULL) {
        // the xinput devices have a class of 0xFF and subclass of 0x5D
        // just in case whatever we messed with breaks, use subclass rather than class
        if (config->interface[0].altsetting->bInterfaceSubClass == 0x5D) {
            if (config->interface[0].altsetting->extra != NULL && config->interface[0].altsetting->extra_length > 6) {
                // controller subtype is at the 5th byte
                subtype = config->interface[0].altsetting->extra[4];
                if (subtype == XINPUT_SUBTYPE_GUITAR ||
                    subtype == XINPUT_SUBTYPE_GUITAR_ALTERNATE ||
                    subtype == XINPUT_SUBTYPE_GUITAR_BASS) {
                    type = RB4_Type_XInputGuitar;
                } else if (subtype == XINPUT_SUBTYPE_DRUM_KIT ||
                           subtype == XINPUT_SUBTYPE_GAMEPAD) { // (for testing purposes)
                    type = RB4_Type_XInputDrums;
                } else {
                    // if we're not a known subtype, assume that we're a wireless dongle (mine has 0x13 at this position)
                    // the subtype of whatever is linked to it has to be asked for separately
                    type = RB4_Type_XInputWireless;
                    subtype = 0;
                }
            }
        }
        sceUsbdFreeConfigDescriptor(config);
    }
    if (subtype_out != NULL)
        *subtype_out = subtype;
    return type;
}

static OIRB4InstrumentKind KindFromSubtype(uint8_t subtype, OIRB4InstrumentKind fallback) {
    switch (subtype) {
        case XINPUT_SUBTYPE_GUITAR:
        case XINPUT_SUBTYPE_GUITAR_ALTERNATE:
        case XINPUT_SUBTYPE_GUITAR_BASS:
            return RB4_Kind_Guitar;
        case XINPUT_SUBTYPE_DRUM_KIT:
            return RB4_Kind_Drums;
        default:
            return fallback;
    }
}

// finds the interrupt OUT endpoint of the controller interface, 0 if there isn't one
static uint8_t FindOutEndpoint(libusb_device *device) {
    struct libusb_config_descriptor *config = NULL;
    uint8_t endpoint = 0;
    if (sceUsbdGetConfigDescriptor(device, 0, &config) == 0 && config != NULL) {
        const struct libusb_interface_descriptor *interface = &config->interface[0].altsetting[0];
        for (int i = 0; i < interface->bNumEndpoints && interface->endpoint != NULL; i++) {
            const struct libusb_endpoint_descriptor *ep = &interface->endpoint[i];
            if ((ep->bEndpointAddress & 0x80) == 0 && (ep->bmAttributes & 0x03) == 0x03) {
                endpoint = ep->bEndpointAddress;
                break;
            }
        }
        sceUsbdFreeConfigDescriptor(config);
    }
    // anything else means the descriptor layout isn't what we expect, so don't touch it
    if (endpoint == 0 || endpoint > 0x0F)
        return 0;
    return endpoint;
}

static OIRB4WirelessReceiver *FindWirelessReceiver(libusb_device *device) {
    uint8_t bus = sceUsbdGetBusNumber(device);
    uint8_t address = sceUsbdGetDeviceAddress(device);
    for (int i = 0; i < MAX_WIRELESS_RECEIVERS; i++) {
        OIRB4WirelessReceiver *receiver = &wireless_receivers[i];
        if (receiver->device == device && receiver->bus == bus && receiver->address == address)
            return receiver;
    }
    return NULL;
}

// returns the controller subtype from a receiver link report, 0 if this isn't one
static uint8_t ParseWirelessLinkReport(const uint8_t *data, int length) {
    if (length > XINPUT_WIRELESS_LINK_SUBTYPE &&
        data[0] == XINPUT_WIRELESS_ID_DATA &&
        data[1] == XINPUT_WIRELESS_TYPE_LINK &&
        data[5] == XINPUT_WIRELESS_LINK_ID)
        return data[XINPUT_WIRELESS_LINK_SUBTYPE] & 0x7F;
    return 0;
}

// Asks a wireless receiver what's linked to its first slot. Blocks for up to about a second,
// so only do it once per device. Returns the XInput subtype, or 0 if nothing answered.
static uint8_t ProbeWirelessSubtype(libusb_device *device) {
    libusb_device_handle *handle = NULL;
    uint8_t subtype = 0;
    if (sceUsbdOpen(device, &handle) != 0 || handle == NULL)
        return 0;
    if (sceUsbdClaimInterface(handle, 0) == 0) {
        // device connection request (MS-XUSBI 3.5.5.2.3): the receiver answers with
        // the connection status, then the link report which holds the subtype
        uint8_t request[XINPUT_OUT_BUFFER_SIZE] = { 0x08, 0x00, 0x0F, 0xC0 };
        int transferred = 0;
        if (sceUsbdInterruptTransfer(handle, XINPUT_WIRELESS_OUT_ENDPOINT, request, sizeof(request), &transferred, 100) == 0) {
            uint8_t packet[32];
            // genuine receivers constantly send empty packets, so go by time rather than packet count
            uint64_t now = sceKernelGetProcessTime();
            const uint64_t give_up = now + 1500000;
            uint64_t deadline = now + 1000000;
            while (subtype == 0 && now < deadline && now < give_up) {
                int length = 0;
                if (sceUsbdInterruptTransfer(handle, XINPUT_WIRELESS_IN_ENDPOINT, packet, sizeof(packet), &length, 50) == 0) {
                    if (length >= 2 && packet[0] == XINPUT_WIRELESS_ID_CONNECTION) {
                        if ((packet[1] & XINPUT_WIRELESS_CONNECTED_DATA) == 0) {
                            // nothing is linked to the receiver, so there's no link report to wait for,
                            // but give it a moment in case that was a stale status from before it linked
                            if (deadline > now + 200000)
                                deadline = now + 200000;
                        } else if (deadline < now + 500000) {
                            // something is linked, the link report should follow
                            deadline = now + 500000;
                        }
                    } else {
                        subtype = ParseWirelessLinkReport(packet, length);
                    }
                }
                now = sceKernelGetProcessTime();
            }
        }
        sceUsbdReleaseInterface(handle, 0);
    }
    sceUsbdClose(handle);
    return subtype;
}

// works out what to tell the game a wireless receiver is, only asking the receiver the first time
static OIRB4WirelessReceiver *IdentifyWirelessReceiver(libusb_device *device) {
    OIRB4WirelessReceiver *receiver = FindWirelessReceiver(device);
    if (receiver != NULL)
        return receiver;

    uint8_t subtype = ProbeWirelessSubtype(device);
    final_printf("Wireless receiver probe: subtype 0x%02X (pending 0x%02X)\n", subtype, pending_wireless_subtype);
    if (subtype == 0)
        subtype = pending_wireless_subtype;
    pending_wireless_subtype = 0;

    receiver = &wireless_receivers[next_wireless_receiver];
    next_wireless_receiver = (next_wireless_receiver + 1) % MAX_WIRELESS_RECEIVERS;
    receiver->device = device;
    receiver->bus = sceUsbdGetBusNumber(device);
    receiver->address = sceUsbdGetDeviceAddress(device);
    receiver->subtype = subtype;
    receiver->kind = KindFromSubtype(subtype, RB4_WIRELESS_DEFAULT_KIND);
    return receiver;
}

static void OutTransferCallback(struct libusb_transfer *transfer) {
    OIRB4OpenDevice *device = (OIRB4OpenDevice *)transfer->user_data;
    if (device != NULL) {
        device->out_pending = false;
        // try again on the next input packet
        if (transfer->status != LIBUSB_TRANSFER_COMPLETED)
            device->led_set = false;
    }
    sceUsbdFreeTransfer(transfer);
}

static bool SendOut(OIRB4OpenDevice *device, const uint8_t *data, int length) {
    if (device->out_endpoint == 0 || device->out_pending || length > XINPUT_OUT_BUFFER_SIZE)
        return false;
    struct libusb_transfer *transfer = sceUsbdAllocTransfer(0);
    if (transfer == NULL)
        return false;
    memcpy(device->out_buffer, data, length);
    sceUsbdFillInterruptTransfer(transfer, device->device_handle, device->out_endpoint, device->out_buffer, length, OutTransferCallback, device, 500);
    device->out_pending = true;
    if (sceUsbdSubmitTransfer(transfer) != 0) {
        device->out_pending = false;
        sceUsbdFreeTransfer(transfer);
        return false;
    }
    return true;
}

// Lights up the player LED. Stops the eternal flashing, and wireless controllers
// drop their link again shortly after relinking if they don't get one.
static void SetPlayerLed(OIRB4OpenDevice *device) {
    // 0x06-0x09: flash, then light up the LED for players 1-4
    uint8_t pattern = 0x06 + (uint8_t)((device - open_devices) % 4);
    uint8_t request[XINPUT_OUT_BUFFER_SIZE] = { 0 };
    int length;
    if (device->type == RB4_Type_XInputWireless) {
        request[2] = 0x08;
        request[3] = 0x40 | pattern;
        length = XINPUT_OUT_BUFFER_SIZE;
    } else {
        request[0] = 0x01;
        request[1] = 0x03;
        request[2] = pattern;
        length = 3;
    }
    if (SendOut(device, request, length))
        device->led_set = true;
}

static void HandleWirelessLink(OIRB4OpenDevice *device, uint8_t subtype) {
    device->subtype = subtype;
    device->led_set = false;

    // the game was already told what this is, it'll only find out otherwise when the receiver is replugged
    OIRB4InstrumentKind kind = KindFromSubtype(subtype, device->kind);
    if (kind != device->kind && !device->mismatch_notified) {
        device->mismatch_notified = true;
        pending_wireless_subtype = subtype;
        final_printf("Wireless link: subtype 0x%02X doesn't match what the game was told\n", subtype);
        if (kind == RB4_Kind_Guitar)
            DoNotification("Guitar linked to a receiver set up as drums.\nReplug the receiver to fix it.");
        else
            DoNotification("Drums linked to a receiver set up as a guitar.\nReplug the receiver to fix it.");
    }
}

static void HandleWirelessPacket(OIRB4OpenDevice *device, const uint8_t *data, int length) {
    if (length < 2)
        return;
    if (data[0] == XINPUT_WIRELESS_ID_CONNECTION) {
        if ((data[1] & XINPUT_WIRELESS_CONNECTED_DATA) == 0) {
            // controller went away, let go of everything
            device->has_input = false;
            device->led_set = false;
        }
        return;
    }
    if (data[0] != XINPUT_WIRELESS_ID_DATA)
        return;
    if (data[1] == XINPUT_WIRELESS_TYPE_CONTROLS || data[1] == XINPUT_WIRELESS_TYPE_CONTROLS_PMD) {
        // sometimes the wireless report will just be a silly nothingpacket,
        // so only keep the ones that actually have input data in them
        if (length >= XINPUT_WIRELESS_CONTROLS_OFFSET + XINPUT_CONTROLS_SIZE) {
            memcpy(device->last_controls, data + XINPUT_WIRELESS_CONTROLS_OFFSET, XINPUT_CONTROLS_SIZE);
            device->has_input = true;
        }
    } else if (data[1] == XINPUT_WIRELESS_TYPE_LINK) {
        uint8_t subtype = ParseWirelessLinkReport(data, length);
        if (subtype != 0)
            HandleWirelessLink(device, subtype);
    }
}

static void HandleWiredPacket(OIRB4OpenDevice *device, const uint8_t *data, int length) {
    // wired controllers also send LED and rumble status messages, ignore those
    if (length >= XINPUT_CONTROLS_SIZE && data[0] == XINPUT_MESSAGE_CONTROLS && data[1] >= XINPUT_CONTROLS_SIZE) {
        memcpy(device->last_controls, data, XINPUT_CONTROLS_SIZE);
        device->has_input = true;
    }
}

#define BIT(i) (1 << (i))
#define PS3_BUTTON_SQUARE   BIT(0)
#define PS3_BUTTON_CROSS    BIT(1)
#define PS3_BUTTON_CIRCLE   BIT(2)
#define PS3_BUTTON_TRIANGLE BIT(3)
#define PS3_BUTTON_L1       BIT(4)
#define PS3_BUTTON_R1       BIT(5)
#define PS3_BUTTON_L2       BIT(6)
#define PS3_BUTTON_R2       BIT(7)
#define PS3_BUTTON_SELECT   BIT(8)
#define PS3_BUTTON_START    BIT(9)
#define PS3_BUTTON_L3       BIT(10)
#define PS3_BUTTON_R3       BIT(11)
#define PS3_BUTTON_PS       BIT(12)

#define PS3_HAT_NEUTRAL 0x08
#define PS3_STICK_CENTER 0x80

// the report sent by PS3 instruments, see the PlasticBand documentation for details
typedef struct _ps3_instrument_report {
    uint16_t buttons;
    uint8_t hat;
    uint8_t left_stick_x;
    uint8_t left_stick_y;
    uint8_t right_stick_x; // guitar: whammy
    uint8_t right_stick_y; // guitar: pickup switch
    uint8_t pressure[12]; // drums: velocities at 4-7
    uint16_t accelerometer[4];
} __attribute__((packed)) ps3_instrument_report;

_Static_assert(sizeof(ps3_instrument_report) == PS3_REPORT_SIZE, "PS3 report must be 27 bytes");

#define PS3_DRUM_VELOCITY_YELLOW 4
#define PS3_DRUM_VELOCITY_RED    5
#define PS3_DRUM_VELOCITY_GREEN  6
#define PS3_DRUM_VELOCITY_BLUE   7

// the tilt sensor reads as an axis on 360 guitars, but a button on PS3 guitars
#define GUITAR_TILT_THRESHOLD 0x4000

static uint8_t HatFromDpad(uint8_t buttons1) {
    bool up = (buttons1 & XINPUT_BUTTON_UP) != 0;
    bool down = (buttons1 & XINPUT_BUTTON_DOWN) != 0;
    bool left = (buttons1 & XINPUT_BUTTON_LEFT) != 0;
    bool right = (buttons1 & XINPUT_BUTTON_RIGHT) != 0;
    if (up && right) return 0x01;
    if (down && right) return 0x03;
    if (down && left) return 0x05;
    if (up && left) return 0x07;
    if (up) return 0x00;
    if (right) return 0x02;
    if (down) return 0x04;
    if (left) return 0x06;
    return PS3_HAT_NEUTRAL;
}

// 360 drum velocities are inverted: red and blue go from 0x0000 (hardest) to 0x7FFF (softest)...
static uint8_t VelocityFromPositiveAxis(int16_t value) {
    if (value < 0)
        return 0;
    return (uint8_t)(value >> 7);
}

// ...and yellow and green go from 0xFFFF (hardest) to 0x8000 (softest). PS3 kits go from 0x00 to 0xFF.
static uint8_t VelocityFromNegativeAxis(int16_t value) {
    if (value >= 0)
        return 0; // RB1 kits don't send velocity, PS3 RB1 kits send 0 for that
    return (uint8_t)((~(uint16_t)value & 0x7FFF) >> 7);
}

static void ConvertDrums(const xinput_report_controls *controls, ps3_instrument_report *report) {
    // pads and cymbals are colour flags plus pad/cymbal flags, with yellow and blue
    // cymbals also pressing up and down on the d-pad, on both platforms
    if ((controls->buttons2 & XINPUT_BUTTON_A) != 0) report->buttons |= PS3_BUTTON_CROSS; // green
    if ((controls->buttons2 & XINPUT_BUTTON_B) != 0) report->buttons |= PS3_BUTTON_CIRCLE; // red
    if ((controls->buttons2 & XINPUT_BUTTON_X) != 0) report->buttons |= PS3_BUTTON_SQUARE; // blue
    if ((controls->buttons2 & XINPUT_BUTTON_Y) != 0) report->buttons |= PS3_BUTTON_TRIANGLE; // yellow
    if ((controls->buttons1 & XINPUT_BUTTON_R3) != 0) report->buttons |= PS3_BUTTON_L3; // pad flag
    if ((controls->buttons2 & XINPUT_BUTTON_RB) != 0) report->buttons |= PS3_BUTTON_R3; // cymbal flag
    // pedals
    if ((controls->buttons2 & XINPUT_BUTTON_LB) != 0) report->buttons |= PS3_BUTTON_L1; // kick 1
    if ((controls->buttons1 & XINPUT_BUTTON_L3) != 0) report->buttons |= PS3_BUTTON_R1; // kick 2
    // velocities
    report->pressure[PS3_DRUM_VELOCITY_RED] = VelocityFromPositiveAxis(controls->left_stick_x);
    report->pressure[PS3_DRUM_VELOCITY_YELLOW] = VelocityFromNegativeAxis(controls->left_stick_y);
    report->pressure[PS3_DRUM_VELOCITY_BLUE] = VelocityFromPositiveAxis(controls->right_stick_x);
    report->pressure[PS3_DRUM_VELOCITY_GREEN] = VelocityFromNegativeAxis(controls->right_stick_y);
}

static void ConvertGuitar(const xinput_report_controls *controls, uint8_t subtype, ps3_instrument_report *report) {
    // frets
    if ((controls->buttons2 & XINPUT_BUTTON_A) != 0) report->buttons |= PS3_BUTTON_CROSS; // green
    if ((controls->buttons2 & XINPUT_BUTTON_B) != 0) report->buttons |= PS3_BUTTON_CIRCLE; // red
    if ((controls->buttons2 & XINPUT_BUTTON_Y) != 0) report->buttons |= PS3_BUTTON_TRIANGLE; // yellow
    if ((controls->buttons2 & XINPUT_BUTTON_X) != 0) report->buttons |= PS3_BUTTON_SQUARE; // blue
    if ((controls->buttons2 & XINPUT_BUTTON_LB) != 0) report->buttons |= PS3_BUTTON_L1; // orange
    // whammy goes from -32768 (released) to 32767 on 360, 0x00 to 0xFF on PS3
    report->right_stick_x = (uint8_t)(((int32_t)controls->right_stick_x + 32768) >> 8);
    // tilt
    if (controls->right_stick_y > GUITAR_TILT_THRESHOLD) report->buttons |= PS3_BUTTON_R1;
    // Guitar Hero guitars use these for an accelerometer and the touch bar instead
    if (subtype != XINPUT_SUBTYPE_GUITAR_ALTERNATE) {
        if ((controls->buttons1 & XINPUT_BUTTON_L3) != 0) report->buttons |= PS3_BUTTON_L2; // solo frets
        report->right_stick_y = controls->left_trigger; // pickup switch
    }
}

static void BuildPS3Report(const OIRB4OpenDevice *device, ps3_instrument_report *report) {
    memset(report, 0, sizeof(*report));
    report->hat = PS3_HAT_NEUTRAL;
    report->left_stick_x = PS3_STICK_CENTER;
    report->left_stick_y = PS3_STICK_CENTER;
    report->right_stick_x = device->kind == RB4_Kind_Guitar ? 0x00 : PS3_STICK_CENTER;
    report->right_stick_y = PS3_STICK_CENTER;
    if (!device->has_input)
        return;

    const xinput_report_controls *controls = (const xinput_report_controls *)device->last_controls;
    if ((controls->buttons1 & XINPUT_BUTTON_BACK) != 0) report->buttons |= PS3_BUTTON_SELECT;
    if ((controls->buttons1 & XINPUT_BUTTON_START) != 0) report->buttons |= PS3_BUTTON_START;
    if ((controls->buttons2 & XINPUT_BUTTON_GUIDE) != 0) report->buttons |= PS3_BUTTON_PS;
    // d-pad, strum bar on guitars
    report->hat = HatFromDpad(controls->buttons1);

    if (device->kind == RB4_Kind_Drums)
        ConvertDrums(controls, report);
    else
        ConvertGuitar(controls, device->subtype, report);
}

libusb_transfer_cb_fn interrupt_callback;
void ParseXInputCallback(struct libusb_transfer *transfer) {
    //final_printf("ParseXInputCallback\n");
    OIRB4OpenDevice *device = GetOpenDeviceFromDeviceHandle(transfer->dev_handle);
    if (device != NULL && transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        int length = transfer->actual_length;
        if (length > transfer->length)
            length = transfer->length;

        if (device->type == RB4_Type_XInputWireless)
            HandleWirelessPacket(device, transfer->buffer, length);
        else
            HandleWiredPacket(device, transfer->buffer, length);

        // as soon as something is known to be linked, not on the first button press,
        // since a relinked wireless controller drops again if it waits too long
        if (!device->led_set && (device->has_input || device->subtype != 0))
            SetPlayerLed(device);

        ps3_instrument_report report;
        BuildPS3Report(device, &report);
        int report_length = transfer->length < PS3_REPORT_SIZE ? transfer->length : PS3_REPORT_SIZE;
        memcpy(transfer->buffer, &report, report_length);
        transfer->actual_length = report_length;
    }
    interrupt_callback(transfer);
}

int TsceUsbdOpen_hook(libusb_device *device, libusb_device_handle **dev_handle) {
    //final_printf("sceUsbdOpen_hook\n");
    int r = sceUsbdOpen(device, dev_handle);
    if (r == 0 && dev_handle != NULL && *dev_handle != NULL) {
        uint8_t subtype = 0;
        OIRB4DeviceType type = IdentifyDevice(device, &subtype);
        if (type != RB4_Type_None) {
            OIRB4OpenDevice *opendevice = GetOpenDeviceFromDevice(device);
            if (opendevice == NULL)
                return r;
            opendevice->type = type;
            opendevice->device_handle = *dev_handle;
            opendevice->has_input = false;
            opendevice->led_set = false;
            opendevice->out_pending = false;
            opendevice->mismatch_notified = false;
            if (type == RB4_Type_XInputWireless) {
                // use what the game was told when it read the device descriptor
                OIRB4WirelessReceiver *receiver = IdentifyWirelessReceiver(device);
                opendevice->kind = receiver->kind;
                opendevice->subtype = receiver->subtype;
                opendevice->out_endpoint = XINPUT_WIRELESS_OUT_ENDPOINT;
            } else {
                opendevice->kind = type == RB4_Type_XInputGuitar ? RB4_Kind_Guitar : RB4_Kind_Drums;
                opendevice->subtype = subtype;
                opendevice->out_endpoint = FindOutEndpoint(device);
            }
            final_printf("Opened XInput device: type %d, kind %d, subtype 0x%02X, out endpoint 0x%02X\n",
                opendevice->type, opendevice->kind, opendevice->subtype, opendevice->out_endpoint);
        }
    }
    return r;
}

void TsceUsbdClose_hook(libusb_device_handle *dev_handle) {
    //final_printf("sceUsbdClose_hook\n");
    if (dev_handle == NULL)
        return;
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
            OIRB4InstrumentKind kind;
            bool is_instrument = true;
            switch (IdentifyDevice(device, NULL)) {
                case RB4_Type_XInputGuitar:
                    kind = RB4_Kind_Guitar;
                    break;
                case RB4_Type_XInputDrums:
                    kind = RB4_Kind_Drums;
                    break;
                case RB4_Type_XInputWireless:
                    kind = IdentifyWirelessReceiver(device)->kind;
                    break;
                default:
                    is_instrument = false;
                    break;
            }
            if (is_instrument) {
                desc->idVendor = 0x12BA;
                desc->idProduct = kind == RB4_Kind_Guitar ? 0x0200 : 0x0210;
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
