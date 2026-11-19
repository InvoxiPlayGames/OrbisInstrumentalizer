/*
    usbd_hooks_rb4.c - OrbisInstrumentalizer
    Hooks to the libSceUsbd library to detect XInput instruments.
    Designed for use with Rock Band 4.
    Licensed under the GNU Lesser General Public License version 2.1, or later.

    Wired XInput instruments are passed to the game as they are, with their descriptors
    and input reports rewritten to look like PS3 instruments.

    Xbox 360 wireless receivers have 4 controller slots, but the game would only ever read
    the first one. So receivers are hidden from the game: the plugin opens them itself, and
    every slot with an instrument linked to it is shown to the game as its own virtual device.
*/

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <GoldHEN/Common.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>
// uncomment when OpenOrbis merges #229
//#include <orbis/Usbd.h>
#include "OrbisUsbd.h"
#include "xinput.h"
#include "oi_log.h"

#define final_printf OILog

void DoNotification(const char* text);

// LIBUSB_ERROR_NO_DEVICE in the sceUsbd error range
#define SCE_USBD_ERROR_NO_DEVICE ((int)0x80240004)

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

#define PS3_REPORT_SIZE 27
#define XINPUT_CONTROLS_SIZE ((int)sizeof(xinput_report_controls))
#define XINPUT_OUT_BUFFER_SIZE 12
#define XINPUT_WIRELESS_PACKET_SIZE 32

/* ------------------------------------------------------------------------------------------
    Wireless receivers
------------------------------------------------------------------------------------------ */

#define RECEIVER_SLOTS 4
#define MAX_RECEIVERS 4

typedef struct _OIRB4Receiver OIRB4Receiver;

typedef struct _OIRB4ReceiverSlot {
    OIRB4Receiver *receiver;
    int index;
    // what's linked to the slot
    volatile bool linked;
    volatile uint8_t subtype;
    // The game sees these addresses as the slot's device and handle. There are two device
    // tokens so that a slot that gets unplugged and plugged back in looks like a new device.
    uint8_t device_tokens[2];
    int identity;
    uint8_t handle_token;
    uint8_t address; // made up, but in the valid range and not used by a real device
    OIRB4InstrumentKind kind; // what the game was told this slot is
    bool open; // the game has the slot's virtual device open
    int open_count; // the game may open the same device more than once
    volatile bool unplugging; // pretending to be unplugged until the game closes it
    // We're the only ones reading the slot: two transfers on the same endpoint (and cancelling
    // one of them) stalls it. The game's transfers wait here for the next packet instead.
    struct libusb_transfer *watcher;
    volatile bool watching;
    uint8_t watch_buffer[XINPUT_WIRELESS_PACKET_SIZE];
#define MAX_PENDING_GAME_TRANSFERS 4
    struct libusb_transfer *pending[MAX_PENDING_GAME_TRANSFERS];
    int pending_count;
    // player LED
    volatile bool led_pending;
    uint8_t led_buffer[XINPUT_OUT_BUFFER_SIZE];
} OIRB4ReceiverSlot;

struct _OIRB4Receiver {
    bool in_use;
    bool present; // in the last device list the game got
    // device pointers aren't guaranteed to stay the same between device lists, these are
    uint8_t bus;
    uint8_t address;
    libusb_device *device; // the one our handle holds on to
    libusb_device_handle *handle; // opened by us, shared by all the slots
    OIRB4ReceiverSlot slots[RECEIVER_SLOTS];
};

static OIRB4Receiver receivers[MAX_RECEIVERS] = { 0 };

// defined further down, once open devices and reports are
static void SlotPacketArrived(OIRB4ReceiverSlot *slot, const uint8_t *data, int length);
static void FailPendingGameTransfers(OIRB4ReceiverSlot *slot, int status);
static void CompleteDeferredGameTransfers(void);


static uint8_t SlotInEndpoint(const OIRB4ReceiverSlot *slot) { return 0x81 + 2 * slot->index; }
static uint8_t SlotOutEndpoint(const OIRB4ReceiverSlot *slot) { return 0x01 + 2 * slot->index; }

static libusb_device *SlotDevice(OIRB4ReceiverSlot *slot) {
    return (libusb_device *)&slot->device_tokens[slot->identity];
}

static libusb_device_handle *SlotHandle(OIRB4ReceiverSlot *slot) {
    return (libusb_device_handle *)&slot->handle_token;
}

// Returns the slot a virtual device belongs to, or NULL if it isn't one. Devices the game
// still holds on to from before a slot was replugged are reported as not current.
static OIRB4ReceiverSlot *SlotFromVirtualDevice(const libusb_device *device, bool *current) {
    for (int r = 0; r < MAX_RECEIVERS; r++) {
        for (int s = 0; s < RECEIVER_SLOTS; s++) {
            OIRB4ReceiverSlot *slot = &receivers[r].slots[s];
            for (int id = 0; id < 2; id++) {
                if (device == (const libusb_device *)&slot->device_tokens[id]) {
                    if (current != NULL)
                        *current = receivers[r].in_use && id == slot->identity;
                    return slot;
                }
            }
        }
    }
    return NULL;
}

static OIRB4ReceiverSlot *SlotFromVirtualHandle(const libusb_device_handle *handle) {
    for (int r = 0; r < MAX_RECEIVERS; r++) {
        for (int s = 0; s < RECEIVER_SLOTS; s++) {
            if (handle == (const libusb_device_handle *)&receivers[r].slots[s].handle_token)
                return &receivers[r].slots[s];
        }
    }
    return NULL;
}

static bool SlotIsGone(const OIRB4ReceiverSlot *slot) {
    return !slot->receiver->in_use || !slot->receiver->present || slot->unplugging;
}

/* ------------------------------------------------------------------------------------------
    Devices the game has open
------------------------------------------------------------------------------------------ */

typedef struct _OIRB4OpenDevice {
    bool is_open;
    libusb_device *device; // as the game sees it
    libusb_device_handle *device_handle; // as the game sees it
    libusb_device_handle *real_handle; // where transfers actually go
    uint8_t in_endpoint; // the receiver slot's endpoint, 0 for wired devices
    OIRB4ReceiverSlot *slot; // NULL for wired devices
    OIRB4DeviceType type;
    OIRB4InstrumentKind kind;
    uint8_t subtype; // XInput subtype of the instrument, 0 if not known yet
    // Guitar Hero drumkits (5 lanes), which share their subtype with Rock Band ones
    bool five_lane;
    // the last controls received, so non-input packets don't release everything
    bool has_input;
    uint8_t last_controls[sizeof(xinput_report_controls)];
    // player LED on wired devices
    uint8_t out_endpoint;
    bool out_pending;
    bool led_set;
    uint8_t out_buffer[XINPUT_OUT_BUFFER_SIZE];
    // player number the game gave the instrument through its PS3 LED command, 0 if none yet
    int player;
    // for logging input changes
    uint16_t logged_buttons;
    uint8_t logged_hat;
} OIRB4OpenDevice;

#define MAX_DEVICE_COUNT 8
static OIRB4OpenDevice open_devices[MAX_DEVICE_COUNT] = { 0 };

static OIRB4OpenDevice *AllocOpenDevice(libusb_device *device) {
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (!open_devices[i].is_open) {
            memset(&open_devices[i], 0, sizeof(open_devices[i]));
            open_devices[i].is_open = true;
            open_devices[i].device = device;
            return &open_devices[i];
        }
    }
    return NULL;
}

static OIRB4OpenDevice *GetOpenDeviceFromDeviceHandle(libusb_device_handle *device_handle) {
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].is_open && open_devices[i].device_handle == device_handle)
            return &open_devices[i];
    }
    return NULL;
}

static int SlotPlayer(OIRB4ReceiverSlot *slot) {
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].is_open && open_devices[i].slot == slot)
            return open_devices[i].player;
    }
    return 0;
}

// The game's own callback and handle for each of its transfers to our devices. Every one of
// them has to make it back to the game with the handle it knows, even after the device is
// closed (cancellations), or the game can end up waiting on it forever.
#define MAX_GAME_TRANSFERS 64
static struct {
    struct libusb_transfer *transfer;
    libusb_transfer_cb_fn callback;
    libusb_device_handle *game_handle;
} game_transfers[MAX_GAME_TRANSFERS] = { 0 };
static int next_game_transfer = 0;

static void RememberGameTransfer(struct libusb_transfer *transfer, libusb_transfer_cb_fn callback, libusb_device_handle *game_handle) {
    int index = -1;
    for (int i = 0; i < MAX_GAME_TRANSFERS && index < 0; i++) {
        if (game_transfers[i].transfer == transfer)
            index = i;
    }
    if (index < 0) {
        // transfers get reused, so there's no telling when one is gone for good; recycle the oldest
        index = next_game_transfer;
        next_game_transfer = (next_game_transfer + 1) % MAX_GAME_TRANSFERS;
    }
    game_transfers[index].transfer = transfer;
    game_transfers[index].callback = callback;
    game_transfers[index].game_handle = game_handle;
}

static bool LookUpGameTransfer(const struct libusb_transfer *transfer, libusb_transfer_cb_fn *callback, libusb_device_handle **game_handle) {
    for (int i = 0; i < MAX_GAME_TRANSFERS; i++) {
        if (game_transfers[i].transfer == transfer) {
            *callback = game_transfers[i].callback;
            *game_handle = game_transfers[i].game_handle;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------------------------
    Identifying devices
------------------------------------------------------------------------------------------ */

static OIRB4DeviceType IdentifyDevice(libusb_device *device, uint8_t *subtype_out) {
    struct libusb_config_descriptor *config = NULL;
    OIRB4DeviceType type = RB4_Type_None;
    uint8_t subtype = 0;
    if (sceUsbdGetConfigDescriptor(device, 0, &config) == 0 && config != NULL) {
        const struct libusb_interface_descriptor *interface = config->interface[0].altsetting;
        // the xinput devices have a class of 0xFF and subclass of 0x5D
        // just in case whatever we messed with breaks, use subclass rather than class
        if (interface->bInterfaceSubClass == 0x5D) {
            if (interface->bInterfaceProtocol == 0x81) {
                // wireless receivers use protocol 0x81, what's linked to them is found out separately
                type = RB4_Type_XInputWireless;
            } else if (interface->extra != NULL && interface->extra_length > 6) {
                // controller subtype is at the 5th byte
                subtype = interface->extra[4];
                if (subtype == XINPUT_SUBTYPE_GUITAR ||
                    subtype == XINPUT_SUBTYPE_GUITAR_ALTERNATE ||
                    subtype == XINPUT_SUBTYPE_GUITAR_BASS) {
                    type = RB4_Type_XInputGuitar;
                } else if (subtype == XINPUT_SUBTYPE_DRUM_KIT ||
                           subtype == XINPUT_SUBTYPE_GAMEPAD) { // (for testing purposes)
                    type = RB4_Type_XInputDrums;
                }
            }
        }
        sceUsbdFreeConfigDescriptor(config);
    }
    if (subtype_out != NULL)
        *subtype_out = subtype;
    return type;
}

static bool IsXInputDevice(libusb_device *device) {
    struct libusb_device_descriptor desc;
    if (sceUsbdGetDeviceDescriptor(device, &desc) != 0)
        return false;
    return desc.bDeviceClass == 0xFF && desc.bDeviceSubClass == 0xFF && desc.bDeviceProtocol == 0xFF;
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

static void SetPS3InstrumentIds(struct libusb_device_descriptor *desc, OIRB4InstrumentKind kind) {
    desc->idVendor = 0x12BA; // licensed by SCEA
    desc->idProduct = kind == RB4_Kind_Guitar ? 0x0200 : 0x0210;
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

/* ------------------------------------------------------------------------------------------
    Wireless receiver slots: link status, LEDs, watching
------------------------------------------------------------------------------------------ */

// returns the controller subtype from a receiver link report, 0 if this isn't one
static uint8_t ParseWirelessLinkReport(const uint8_t *data, int length) {
    if (length > XINPUT_WIRELESS_LINK_SUBTYPE &&
        data[0] == XINPUT_WIRELESS_ID_DATA &&
        data[1] == XINPUT_WIRELESS_TYPE_LINK &&
        data[5] == XINPUT_WIRELESS_LINK_ID)
        return data[XINPUT_WIRELESS_LINK_SUBTYPE] & 0x7F;
    return 0;
}

static void SlotLedCallback(struct libusb_transfer *transfer) {
    OIRB4ReceiverSlot *slot = (OIRB4ReceiverSlot *)transfer->user_data;
    slot->led_pending = false;
    sceUsbdFreeTransfer(transfer);
}

static void SetSlotLed(OIRB4ReceiverSlot *slot);

// player number the game gave a slot, 0 if it hasn't
static int SlotPlayer(OIRB4ReceiverSlot *slot);

// Lights up the quarter of the ring for the player number the game gave the instrument, or
// for the slot until it does. Needed as soon as a controller links, as wireless controllers
// drop their link again shortly after if they don't get one.
static void SetSlotLed(OIRB4ReceiverSlot *slot) {
    if (slot->led_pending || !slot->receiver->in_use)
        return;
    int player = SlotPlayer(slot);
    int position = player >= 1 && player <= 4 ? player - 1 : slot->index;
    struct libusb_transfer *transfer = sceUsbdAllocTransfer(0);
    if (transfer == NULL)
        return;
    memset(slot->led_buffer, 0, sizeof(slot->led_buffer));
    slot->led_buffer[2] = 0x08;
    slot->led_buffer[3] = 0x40 | (0x06 + position); // 0x06-0x09: flash, then light up players 1-4
    sceUsbdFillInterruptTransfer(transfer, slot->receiver->handle, SlotOutEndpoint(slot), slot->led_buffer,
        sizeof(slot->led_buffer), SlotLedCallback, slot, 500);
    slot->led_pending = true;
    if (sceUsbdSubmitTransfer(transfer) != 0) {
        slot->led_pending = false;
        sceUsbdFreeTransfer(transfer);
    }
}

// handles a link report or connection status that came from a slot, whoever was reading it
static void HandleSlotLinkPacket(OIRB4ReceiverSlot *slot, const uint8_t *data, int length) {
    if (length < 2)
        return;
    if (data[0] == XINPUT_WIRELESS_ID_CONNECTION) {
        if ((data[1] & XINPUT_WIRELESS_CONNECTED_DATA) == 0)
            slot->linked = false;
        return;
    }
    uint8_t subtype = ParseWirelessLinkReport(data, length);
    if (subtype == 0)
        return;
    final_printf("Receiver slot %d: subtype 0x%02X linked\n", slot->index + 1, subtype);
    slot->subtype = subtype;
    slot->linked = true;
    SetSlotLed(slot);
    // the game was told this is something else, so unplug it and let it come back as the right thing
    if (slot->open && KindFromSubtype(subtype, slot->kind) != slot->kind) {
        final_printf("Receiver slot %d: replugging as a different instrument\n", slot->index + 1);
        slot->unplugging = true;
        FailPendingGameTransfers(slot, LIBUSB_TRANSFER_NO_DEVICE);
    }
}

static void WatcherCallback(struct libusb_transfer *transfer) {
    OIRB4ReceiverSlot *slot = (OIRB4ReceiverSlot *)transfer->user_data;
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        SlotPacketArrived(slot, transfer->buffer, transfer->actual_length);
        CompleteDeferredGameTransfers();
        if (slot->watching && sceUsbdSubmitTransfer(transfer) == 0)
            return;
    }
    OILog("Stopped reading receiver slot %d (status %d)\n", slot->index + 1, transfer->status);
    slot->watching = false;
    slot->watcher = NULL;
    sceUsbdFreeTransfer(transfer);
    // nothing will answer the game's transfers anymore
    FailPendingGameTransfers(slot, LIBUSB_TRANSFER_NO_DEVICE);
}

static void StartWatching(OIRB4ReceiverSlot *slot) {
    if (slot->watcher != NULL || !slot->receiver->in_use || !slot->receiver->present)
        return;
    struct libusb_transfer *transfer = sceUsbdAllocTransfer(0);
    if (transfer == NULL)
        return;
    sceUsbdFillInterruptTransfer(transfer, slot->receiver->handle, SlotInEndpoint(slot), slot->watch_buffer,
        sizeof(slot->watch_buffer), WatcherCallback, slot, 0);
    slot->watching = true;
    slot->watcher = transfer;
    if (sceUsbdSubmitTransfer(transfer) != 0) {
        slot->watching = false;
        slot->watcher = NULL;
        sceUsbdFreeTransfer(transfer);
    }
}

// the watcher frees itself once the cancellation comes through
static void StopWatching(OIRB4ReceiverSlot *slot) {
    slot->watching = false;
    if (slot->watcher != NULL)
        sceUsbdCancelTransfer(slot->watcher);
}

// Asks a receiver what's linked to each of its slots. Blocks for up to about a second,
// so it's only done when a receiver first shows up; watchers take it from there.
static void ProbeReceiver(OIRB4Receiver *receiver) {
    // device connection request (MS-XUSBI 3.5.5.2.3): the receiver answers with
    // the connection status, then the link report which holds the subtype
    uint8_t request[XINPUT_OUT_BUFFER_SIZE] = { 0x08, 0x00, 0x0F, 0xC0 };
    bool done[RECEIVER_SLOTS] = { false };
    for (int s = 0; s < RECEIVER_SLOTS; s++) {
        OIRB4ReceiverSlot *slot = &receiver->slots[s];
        int transferred = 0;
        sceUsbdClaimInterface(receiver->handle, 2 * s);
        if (sceUsbdInterruptTransfer(receiver->handle, SlotOutEndpoint(slot), request, sizeof(request), &transferred, 100) != 0)
            done[s] = true;
    }

    uint8_t packet[XINPUT_WIRELESS_PACKET_SIZE];
    uint64_t now = sceKernelGetProcessTime();
    const uint64_t give_up = now + 1500000;
    uint64_t deadline = now + 1000000;
    bool all_done = false;
    while (!all_done && now < deadline && now < give_up) {
        all_done = true;
        for (int s = 0; s < RECEIVER_SLOTS; s++) {
            if (done[s])
                continue;
            all_done = false;
            OIRB4ReceiverSlot *slot = &receiver->slots[s];
            int length = 0;
            if (sceUsbdInterruptTransfer(receiver->handle, SlotInEndpoint(slot), packet, sizeof(packet), &length, 20) != 0)
                continue;
            if (length >= 2 && packet[0] == XINPUT_WIRELESS_ID_CONNECTION) {
                if ((packet[1] & XINPUT_WIRELESS_CONNECTED_DATA) == 0) {
                    done[s] = true; // nothing linked, the watcher will notice if something links later
                } else if (deadline < now + 500000) {
                    deadline = now + 500000; // something is linked, the link report should follow
                }
            } else if (ParseWirelessLinkReport(packet, length) != 0) {
                HandleSlotLinkPacket(slot, packet, length);
                done[s] = true;
            }
        }
        now = sceKernelGetProcessTime();
    }
}

static OIRB4Receiver *FindReceiver(libusb_device *device) {
    uint8_t bus = sceUsbdGetBusNumber(device);
    uint8_t address = sceUsbdGetDeviceAddress(device);
    for (int r = 0; r < MAX_RECEIVERS; r++) {
        if (receivers[r].in_use && receivers[r].bus == bus && receivers[r].address == address)
            return &receivers[r];
    }
    return NULL;
}

static OIRB4Receiver *AddReceiver(libusb_device *device) {
    OIRB4Receiver *receiver = NULL;
    for (int r = 0; r < MAX_RECEIVERS && receiver == NULL; r++) {
        if (!receivers[r].in_use)
            receiver = &receivers[r];
    }
    if (receiver == NULL)
        return NULL;

    libusb_device_handle *handle = NULL;
    if (sceUsbdOpen(device, &handle) != 0 || handle == NULL) {
        final_printf("Couldn't open a wireless receiver\n");
        return NULL;
    }
    // keep the device tokens' identities, so devices the game might still hold stay stale
    for (int s = 0; s < RECEIVER_SLOTS; s++) {
        OIRB4ReceiverSlot *slot = &receiver->slots[s];
        int identity = slot->identity;
        memset(slot, 0, sizeof(*slot));
        slot->receiver = receiver;
        slot->index = s;
        slot->identity = identity ^ 1;
    }
    // the device our handle holds a reference to, the game's list may be freed at any point
    libusb_device *own_device = sceUsbdGetDevice(handle);
    receiver->device = own_device != NULL ? own_device : device;
    receiver->bus = sceUsbdGetBusNumber(device);
    receiver->address = sceUsbdGetDeviceAddress(device);
    receiver->handle = handle;
    receiver->present = true;
    receiver->in_use = true;

    uint64_t start = sceKernelGetProcessTime();
    ProbeReceiver(receiver);
    final_printf("Wireless receiver added (bus %u, address %u), probed in %lu ms: slots %02X %02X %02X %02X\n",
        receiver->bus, receiver->address, (unsigned long)((sceKernelGetProcessTime() - start) / 1000),
        receiver->slots[0].subtype, receiver->slots[1].subtype, receiver->slots[2].subtype, receiver->slots[3].subtype);
    for (int s = 0; s < RECEIVER_SLOTS; s++)
        StartWatching(&receiver->slots[s]);
    return receiver;
}

// lets go of receivers that were unplugged, once nothing is using them anymore
static void CleanUpReceivers(void) {
    for (int r = 0; r < MAX_RECEIVERS; r++) {
        OIRB4Receiver *receiver = &receivers[r];
        if (!receiver->in_use || receiver->present)
            continue;
        bool busy = false;
        for (int s = 0; s < RECEIVER_SLOTS; s++) {
            OIRB4ReceiverSlot *slot = &receiver->slots[s];
            StopWatching(slot);
            FailPendingGameTransfers(slot, LIBUSB_TRANSFER_NO_DEVICE);
            if (slot->open || slot->watcher != NULL || slot->led_pending)
                busy = true;
        }
        if (busy)
            continue;
        sceUsbdClose(receiver->handle);
        receiver->handle = NULL;
        receiver->device = NULL;
        receiver->in_use = false;
        final_printf("Wireless receiver removed\n");
    }
}

static bool SlotIsVisible(const OIRB4ReceiverSlot *slot) {
    if (slot->unplugging)
        return false;
    // an open slot stays put when its instrument is turned off, like a PS3 dongle would
    return slot->open || slot->linked;
}

/* ------------------------------------------------------------------------------------------
    Device lists: swapping receivers for their slots' virtual devices
------------------------------------------------------------------------------------------ */

#define MAX_DEVICE_LISTS 32
static struct {
    libusb_device **replacement;
    libusb_device **original;
} device_lists[MAX_DEVICE_LISTS] = { 0 };

static bool RememberDeviceList(libusb_device **replacement, libusb_device **original) {
    for (int i = 0; i < MAX_DEVICE_LISTS; i++) {
        if (device_lists[i].replacement == NULL) {
            device_lists[i].replacement = replacement;
            device_lists[i].original = original;
            return true;
        }
    }
    return false;
}

static libusb_device **ForgetDeviceList(libusb_device **replacement) {
    for (int i = 0; i < MAX_DEVICE_LISTS; i++) {
        if (device_lists[i].replacement == replacement) {
            libusb_device **original = device_lists[i].original;
            device_lists[i].replacement = NULL;
            device_lists[i].original = NULL;
            return original;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------
    Packets from the instruments
------------------------------------------------------------------------------------------ */

// Guitar Hero drumkits always hold the left stick click (it's the second kick on Rock Band
// kits), so a kit holding it with nothing else pressed is a Guitar Hero one.
static void DetectFiveLaneDrums(OIRB4OpenDevice *device) {
    if (device->kind != RB4_Kind_Drums || device->five_lane || !device->has_input)
        return;
    const xinput_report_controls *controls = (const xinput_report_controls *)device->last_controls;
    if (controls->buttons1 == XINPUT_BUTTON_L3 && controls->buttons2 == 0) {
        device->five_lane = true;
        final_printf("Guitar Hero drumkit detected\n");
    }
}

static void HandleWirelessPacket(OIRB4OpenDevice *device, const uint8_t *data, int length) {
    if (length < 2)
        return;
    if (data[0] == XINPUT_WIRELESS_ID_CONNECTION) {
        HandleSlotLinkPacket(device->slot, data, length);
        // controller went away, let go of everything
        if (!device->slot->linked)
            device->has_input = false;
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
            DetectFiveLaneDrums(device);
        }
    } else if (data[1] == XINPUT_WIRELESS_TYPE_LINK) {
        HandleSlotLinkPacket(device->slot, data, length);
        device->subtype = device->slot->subtype;
    }
}

static void HandleWiredPacket(OIRB4OpenDevice *device, const uint8_t *data, int length) {
    // wired controllers also send LED and rumble status messages, ignore those
    if (length >= XINPUT_CONTROLS_SIZE && data[0] == XINPUT_MESSAGE_CONTROLS && data[1] >= XINPUT_CONTROLS_SIZE) {
        memcpy(device->last_controls, data, XINPUT_CONTROLS_SIZE);
        device->has_input = true;
        DetectFiveLaneDrums(device);
    }
}

static void WiredLedCallback(struct libusb_transfer *transfer) {
    OIRB4OpenDevice *device = (OIRB4OpenDevice *)transfer->user_data;
    device->out_pending = false;
    // try again on the next input packet
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED)
        device->led_set = false;
    sceUsbdFreeTransfer(transfer);
}

// lights up the player LED on a wired controller, which otherwise flashes forever
static void SetWiredLed(OIRB4OpenDevice *device) {
    if (device->out_endpoint == 0 || device->out_pending)
        return;
    struct libusb_transfer *transfer = sceUsbdAllocTransfer(0);
    if (transfer == NULL)
        return;
    device->out_buffer[0] = 0x01;
    device->out_buffer[1] = 0x03;
    // flash, then light up players 1-4: the game's player number once it gave one
    int position = device->player >= 1 && device->player <= 4 ? device->player - 1 : (int)((device - open_devices) % 4);
    device->out_buffer[2] = 0x06 + (uint8_t)position;
    sceUsbdFillInterruptTransfer(transfer, device->real_handle, device->out_endpoint, device->out_buffer, 3,
        WiredLedCallback, device, 500);
    device->out_pending = true;
    if (sceUsbdSubmitTransfer(transfer) != 0) {
        device->out_pending = false;
        sceUsbdFreeTransfer(transfer);
        return;
    }
    device->led_set = true;
}

/* ------------------------------------------------------------------------------------------
    Converting to PS3 instrument reports
------------------------------------------------------------------------------------------ */

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
#define GH_TILT_THRESHOLD 27000
#define GH_WHAMMY_DEADZONE 0x30

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

// Guitar Hero kits have 3 pads and 2 cymbals, laid out red pad, yellow cymbal, blue pad,
// orange cymbal, green pad. That goes to Rock Band's 4 lanes as yellow and green cymbals.
static void ConvertFiveLaneDrums(const xinput_report_controls *controls, ps3_instrument_report *report) {
    bool red = (controls->buttons2 & XINPUT_BUTTON_B) != 0;
    bool yellow = (controls->buttons2 & XINPUT_BUTTON_Y) != 0;
    bool blue = (controls->buttons2 & XINPUT_BUTTON_X) != 0;
    bool orange = (controls->buttons2 & XINPUT_BUTTON_RB) != 0;
    bool green = (controls->buttons2 & XINPUT_BUTTON_A) != 0;
    if (red) report->buttons |= PS3_BUTTON_CIRCLE | PS3_BUTTON_L3;
    if (blue) report->buttons |= PS3_BUTTON_SQUARE | PS3_BUTTON_L3;
    if (green) report->buttons |= PS3_BUTTON_CROSS | PS3_BUTTON_L3;
    if (yellow) {
        // yellow cymbal: yellow + cymbal flag + d-pad up
        report->buttons |= PS3_BUTTON_TRIANGLE | PS3_BUTTON_R3;
        report->hat = 0x00;
    }
    if (orange) // green cymbal: green + cymbal flag
        report->buttons |= PS3_BUTTON_CROSS | PS3_BUTTON_R3;
    if ((controls->buttons2 & XINPUT_BUTTON_LB) != 0) report->buttons |= PS3_BUTTON_L1; // kick
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
    bool guitar_hero = subtype == XINPUT_SUBTYPE_GUITAR_ALTERNATE;
    // whammy goes from -32768 (released) to 32767 on 360, 0x00 to 0xFF on PS3
    uint8_t whammy = (uint8_t)(((int32_t)controls->right_stick_x + 32768) >> 8);
    // Guitar Hero whammy bars wobble a bit at rest
    if (guitar_hero && whammy < GH_WHAMMY_DEADZONE)
        whammy = 0;
    report->right_stick_x = whammy;
    // tilt: a switch on Rock Band guitars, an accelerometer on Guitar Hero ones, which reads up
    // to about 20000 when held normally and close to 32767 when pointed up
    if (controls->right_stick_y > (guitar_hero ? GH_TILT_THRESHOLD : GUITAR_TILT_THRESHOLD))
        report->buttons |= PS3_BUTTON_R1;
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

    if (device->kind == RB4_Kind_Drums && device->five_lane)
        ConvertFiveLaneDrums(controls, report);
    else if (device->kind == RB4_Kind_Drums)
        ConvertDrums(controls, report);
    else
        ConvertGuitar(controls, device->subtype, report);
}

/* ------------------------------------------------------------------------------------------
    PS3 instrument HID requests
------------------------------------------------------------------------------------------ */

#define HID_GET_REPORT 0x01
#define HID_SET_REPORT 0x09
#define HID_REPORT_TYPE_OUTPUT 0x02
#define HID_REPORT_TYPE_FEATURE 0x03
// what the receiver answered the requests it didn't understand with
#define SCE_USBD_ERROR_REQUEST_FAILED ((int)0x80240004)

#define PS3_CAPS_PRESSURE 0x01
#define PS3_CAPS_MOTION   0x02
#define PS3_CAPS_LED      0x04

// Answers the HID class requests the game sends to an instrument like a PS3 one would: the
// capabilities feature report it identifies them with, and the player LED output report.
static int HandlePS3ClassRequest(OIRB4OpenDevice *device, uint8_t type, uint8_t request, uint16_t value,
    unsigned char *data, uint16_t length) {
    uint8_t report_type = value >> 8;
    if ((type & 0x80) != 0) {
        if (request == HID_GET_REPORT && report_type == HID_REPORT_TYPE_FEATURE && data != NULL) {
            // usage 0x2621 (PS3), protocol version 1, then what the instrument supports
            uint8_t capabilities[8] = { 0x21, 0x26, 0x01,
                device->kind == RB4_Kind_Drums ? (PS3_CAPS_PRESSURE | PS3_CAPS_LED) : (PS3_CAPS_MOTION | PS3_CAPS_LED),
                0x00, 0x00, 0x00, 0x00 };
            int count = length < sizeof(capabilities) ? length : sizeof(capabilities);
            memcpy(data, capabilities, count);
            return count;
        }
        return SCE_USBD_ERROR_REQUEST_FAILED;
    }

    if (request == HID_SET_REPORT && report_type == HID_REPORT_TYPE_OUTPUT && data != NULL && length >= 3) {
        const unsigned char *report = data;
        int report_length = length;
        if (report[0] == 0x00 && report_length >= 4) {
            // report id included
            report++;
            report_length--;
        }
        // output type 0x01: player LEDs, one bit per player
        if (report[0] == 0x01) {
            uint8_t leds = report[2] & 0x0F;
            int player = 0;
            for (int i = 0; i < 4 && player == 0; i++) {
                if ((leds & (1 << i)) != 0)
                    player = i + 1;
            }
            // the game keeps sending it every so often, only the changes matter
            if (player != device->player) {
                final_printf("Game set player LED %d on a%s instrument\n", player, device->slot != NULL ? " wireless" : " wired");
                device->player = player;
                if (device->slot != NULL) {
                    SetSlotLed(device->slot);
                } else {
                    device->led_set = false;
                    SetWiredLed(device);
                }
            }
        }
    }
    // everything else sent to an instrument is accepted, there's nothing for a 360 instrument to do with it
    return length;
}

// Every transfer the game makes to one of our devices comes back through here.
void InstrumentTransferCallback(struct libusb_transfer *transfer) {
    libusb_transfer_cb_fn game_callback = NULL;
    libusb_device_handle *game_handle = NULL;
    if (!LookUpGameTransfer(transfer, &game_callback, &game_handle) || game_callback == NULL) {
        // can't happen: only transfers we remembered get this callback
        OILogLimited(10, "Lost track of a transfer (endpoint 0x%02X, status %d)\n", transfer->endpoint, transfer->status);
        return;
    }
    // the device may well be closed already, if this is a cancellation
    OIRB4OpenDevice *device = GetOpenDeviceFromDeviceHandle(game_handle);

    if ((transfer->endpoint & 0x80) == 0) {
        OILogLimited(20, "Game OUT transfer done: endpoint 0x%02X, status %d, length %d\n",
            transfer->endpoint, transfer->status, transfer->actual_length);
    } else if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        OILogLimited(30, "Game IN transfer ended: endpoint 0x%02X, status %d\n", transfer->endpoint, transfer->status);
    }

    if (device != NULL && (transfer->endpoint & 0x80) != 0 && transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        int length = transfer->actual_length;
        if (length > transfer->length)
            length = transfer->length;

        if (device->slot != NULL) {
            HandleWirelessPacket(device, transfer->buffer, length);
        } else {
            HandleWiredPacket(device, transfer->buffer, length);
            if (!device->led_set && device->has_input)
                SetWiredLed(device);
        }

        OILogLimited(20, "Read from %s endpoint 0x%02X: %d bytes, starting %02X %02X\n",
            device->slot != NULL ? "slot" : "wired", transfer->endpoint, length,
            length > 0 ? transfer->buffer[0] : 0, length > 1 ? transfer->buffer[1] : 0);

        ps3_instrument_report report;
        BuildPS3Report(device, &report);
        if (report.buttons != device->logged_buttons || report.hat != device->logged_hat) {
            device->logged_buttons = report.buttons;
            device->logged_hat = report.hat;
            OILogLimited(300, "Input %s %d: buttons 0x%04X, hat %u\n",
                device->slot != NULL ? "slot" : "wired", device->slot != NULL ? device->slot->index + 1 : 0,
                report.buttons, report.hat);
        }
        int report_length = transfer->length < PS3_REPORT_SIZE ? transfer->length : PS3_REPORT_SIZE;
        memcpy(transfer->buffer, &report, report_length);
        transfer->actual_length = report_length;
    }

    // the game only knows the handle it opened, which is a virtual one for receiver slots
    transfer->dev_handle = game_handle;
    OIRB4ReceiverSlot *slot = SlotFromVirtualHandle(game_handle);
    if (slot != NULL && slot->unplugging && transfer->status == LIBUSB_TRANSFER_COMPLETED)
        transfer->status = LIBUSB_TRANSFER_NO_DEVICE;
    game_callback(transfer);
}

/* ------------------------------------------------------------------------------------------
    The game's transfers to receiver slots
------------------------------------------------------------------------------------------ */

// The game's transfers to receiver slots never reach the hardware: they're queued on the slot
// and answered with the next packet our own reader gets. Completions that aren't answers to a
// packet (cancellations, unplugging) wait for the game's next event handling call, as they would
// with a real device, since calling the game back from inside its own Cancel could deadlock it.

static volatile int game_queue_lock = 0;
static void LockGameQueue(void) { while (__sync_lock_test_and_set(&game_queue_lock, 1)) { } }
static void UnlockGameQueue(void) { __sync_lock_release(&game_queue_lock); }

#define MAX_DEFERRED_COMPLETIONS 64
static struct {
    struct libusb_transfer *transfer;
    int status;
    int length;
} deferred_completions[MAX_DEFERRED_COMPLETIONS];
static int deferred_count = 0;

static OIRB4OpenDevice *OpenDeviceForSlot(OIRB4ReceiverSlot *slot) {
    for (int i = 0; i < MAX_DEVICE_COUNT; i++) {
        if (open_devices[i].is_open && open_devices[i].slot == slot)
            return &open_devices[i];
    }
    return NULL;
}

static void LogInputChange(OIRB4OpenDevice *device, const ps3_instrument_report *report) {
    if (report->buttons == device->logged_buttons && report->hat == device->logged_hat)
        return;
    device->logged_buttons = report->buttons;
    device->logged_hat = report->hat;
    const xinput_report_controls *raw = (const xinput_report_controls *)device->last_controls;
    OILogLimited(300, "Input %s %d: buttons 0x%04X, hat %u | raw %02X %02X LT %u RT %u LX %d LY %d RX %d RY %d\n",
        device->slot != NULL ? "slot" : "wired", device->slot != NULL ? device->slot->index + 1 : 0,
        report->buttons, report->hat, raw->buttons1, raw->buttons2, raw->left_trigger, raw->right_trigger,
        raw->left_stick_x, raw->left_stick_y, raw->right_stick_x, raw->right_stick_y);
}

// hands a transfer back to the game, with the instrument's current state if it completed
static void CompleteGameTransfer(struct libusb_transfer *transfer, int status, int length) {
    libusb_transfer_cb_fn game_callback = NULL;
    libusb_device_handle *game_handle = NULL;
    if (!LookUpGameTransfer(transfer, &game_callback, &game_handle) || game_callback == NULL) {
        OILogLimited(10, "Lost track of a slot transfer (status %d)\n", status);
        return;
    }
    OIRB4OpenDevice *device = GetOpenDeviceFromDeviceHandle(game_handle);
    transfer->status = status;
    transfer->actual_length = 0;
    if (status == LIBUSB_TRANSFER_COMPLETED) {
        if ((transfer->endpoint & 0x80) == 0) {
            transfer->actual_length = length; // OUT: as if it was sent
        } else if (device != NULL) {
            ps3_instrument_report report;
            BuildPS3Report(device, &report);
            LogInputChange(device, &report);
            int report_length = transfer->length < PS3_REPORT_SIZE ? transfer->length : PS3_REPORT_SIZE;
            memcpy(transfer->buffer, &report, report_length);
            transfer->actual_length = report_length;
        }
    }
    transfer->dev_handle = game_handle;
    game_callback(transfer);
}

static void DeferGameTransfer(struct libusb_transfer *transfer, int status, int length) {
    LockGameQueue();
    if (deferred_count < MAX_DEFERRED_COMPLETIONS) {
        deferred_completions[deferred_count].transfer = transfer;
        deferred_completions[deferred_count].status = status;
        deferred_completions[deferred_count].length = length;
        deferred_count++;
    } else {
        OILogLimited(5, "Too many deferred transfer completions\n");
    }
    UnlockGameQueue();
}

static void CompleteDeferredGameTransfers(void) {
    struct libusb_transfer *transfers[MAX_DEFERRED_COMPLETIONS];
    int statuses[MAX_DEFERRED_COMPLETIONS];
    int lengths[MAX_DEFERRED_COMPLETIONS];
    LockGameQueue();
    int count = deferred_count;
    for (int i = 0; i < count; i++) {
        transfers[i] = deferred_completions[i].transfer;
        statuses[i] = deferred_completions[i].status;
        lengths[i] = deferred_completions[i].length;
    }
    deferred_count = 0;
    UnlockGameQueue();
    // outside the lock, the game will usually resubmit from its callback
    for (int i = 0; i < count; i++)
        CompleteGameTransfer(transfers[i], statuses[i], lengths[i]);
}

static bool RemovePendingGameTransfer(OIRB4ReceiverSlot *slot, struct libusb_transfer *transfer) {
    bool found = false;
    LockGameQueue();
    for (int i = 0; i < slot->pending_count; i++) {
        if (slot->pending[i] == transfer) {
            for (int j = i; j < slot->pending_count - 1; j++)
                slot->pending[j] = slot->pending[j + 1];
            slot->pending_count--;
            found = true;
            break;
        }
    }
    UnlockGameQueue();
    return found;
}

static void FailPendingGameTransfers(OIRB4ReceiverSlot *slot, int status) {
    struct libusb_transfer *transfers[MAX_PENDING_GAME_TRANSFERS];
    LockGameQueue();
    int count = slot->pending_count;
    for (int i = 0; i < count; i++)
        transfers[i] = slot->pending[i];
    slot->pending_count = 0;
    UnlockGameQueue();
    for (int i = 0; i < count; i++)
        DeferGameTransfer(transfers[i], status, 0);
}

// a packet from a slot, read by our own reader
static void SlotPacketArrived(OIRB4ReceiverSlot *slot, const uint8_t *data, int length) {
    OIRB4OpenDevice *device = OpenDeviceForSlot(slot);
    if (device == NULL) {
        HandleSlotLinkPacket(slot, data, length);
        return;
    }
    HandleWirelessPacket(device, data, length);
    OILogLimited(10, "Read from slot %d: %d bytes, starting %02X %02X\n",
        slot->index + 1, length, length > 0 ? data[0] : 0, length > 1 ? data[1] : 0);

    // answer the game's oldest waiting transfer with the new state
    struct libusb_transfer *transfer = NULL;
    LockGameQueue();
    if (slot->pending_count > 0 && !slot->unplugging) {
        transfer = slot->pending[0];
        for (int i = 0; i < slot->pending_count - 1; i++)
            slot->pending[i] = slot->pending[i + 1];
        slot->pending_count--;
    }
    UnlockGameQueue();
    if (transfer != NULL)
        CompleteGameTransfer(transfer, LIBUSB_TRANSFER_COMPLETED, 0);
}

// the slot a transfer the game set up belongs to, NULL if it's not for a receiver slot
static OIRB4ReceiverSlot *SlotFromGameTransfer(const struct libusb_transfer *transfer) {
    OIRB4ReceiverSlot *slot = SlotFromVirtualHandle(transfer->dev_handle);
    if (slot != NULL)
        return slot;
    libusb_transfer_cb_fn callback;
    libusb_device_handle *game_handle;
    if (LookUpGameTransfer(transfer, &callback, &game_handle))
        return SlotFromVirtualHandle(game_handle);
    return NULL;
}

/* ------------------------------------------------------------------------------------------
    Hooks
------------------------------------------------------------------------------------------ */

int(*TsceUsbdOpen)(int);
int(*TsceUsbdClose)(int);
int(*TsceUsbdGetConfigDescriptor)(int);
int(*TsceUsbdGetDeviceDescriptor)(int);
int(*TsceUsbdFillInterruptTransfer)(int);
int(*TsceUsbdGetDeviceList)(int);
int(*TsceUsbdFreeDeviceList)(int);
int(*TsceUsbdGetBusNumber)(int);
int(*TsceUsbdGetDeviceAddress)(int);
int(*TsceUsbdCheckConnected)(int);
int(*TsceUsbdControlTransfer)(int);
int(*TsceUsbdGetDescriptor)(int);
int(*TsceUsbdGetStringDescriptorAscii)(int);
int(*TsceUsbdSubmitTransfer)(int);
int(*TsceUsbdCancelTransfer)(int);
int(*TsceUsbdHandleEventsTimeout)(int);

HOOK_INIT(TsceUsbdOpen);
HOOK_INIT(TsceUsbdClose);
HOOK_INIT(TsceUsbdGetConfigDescriptor);
HOOK_INIT(TsceUsbdGetDeviceDescriptor);
HOOK_INIT(TsceUsbdFillInterruptTransfer);
HOOK_INIT(TsceUsbdGetDeviceList);
HOOK_INIT(TsceUsbdFreeDeviceList);
HOOK_INIT(TsceUsbdGetBusNumber);
HOOK_INIT(TsceUsbdGetDeviceAddress);
HOOK_INIT(TsceUsbdCheckConnected);
HOOK_INIT(TsceUsbdControlTransfer);
HOOK_INIT(TsceUsbdGetDescriptor);
HOOK_INIT(TsceUsbdGetStringDescriptorAscii);
HOOK_INIT(TsceUsbdSubmitTransfer);
HOOK_INIT(TsceUsbdCancelTransfer);
HOOK_INIT(TsceUsbdHandleEventsTimeout);

// gives every visible slot a USB address in the valid range that no real device has
static void AssignSlotAddresses(libusb_device **devices, int count) {
    bool used[128] = { false };
    used[0] = true;
    for (int i = 0; i < count; i++)
        used[sceUsbdGetDeviceAddress(devices[i]) & 0x7F] = true;
    for (int r = 0; r < MAX_RECEIVERS; r++) {
        for (int s = 0; s < RECEIVER_SLOTS; s++) {
            OIRB4ReceiverSlot *slot = &receivers[r].slots[s];
            if (receivers[r].in_use && slot->address != 0 && !used[slot->address])
                used[slot->address] = true;
            else
                slot->address = 0;
        }
    }
    for (int r = 0; r < MAX_RECEIVERS; r++) {
        for (int s = 0; s < RECEIVER_SLOTS; s++) {
            OIRB4ReceiverSlot *slot = &receivers[r].slots[s];
            if (!receivers[r].in_use || slot->address != 0)
                continue;
            for (int address = 127; address > 0; address--) {
                if (!used[address]) {
                    used[address] = true;
                    slot->address = (uint8_t)address;
                    break;
                }
            }
        }
    }
}

static int device_list_calls = 0;

int TsceUsbdGetDeviceList_hook(libusb_device ***list) {
    uint64_t start = sceKernelGetProcessTime();
    int count = sceUsbdGetDeviceList(list);
    device_list_calls++;
    if (count < 0 || list == NULL || *list == NULL)
        return count;
    libusb_device **original = *list;

    for (int r = 0; r < MAX_RECEIVERS; r++)
        receivers[r].present = false;
    if (count == 0) {
        CleanUpReceivers();
        return count;
    }

    libusb_device **replacement = malloc(sizeof(libusb_device *) * (count * RECEIVER_SLOTS + 1));
    if (replacement == NULL)
        return count;
    int replacement_count = 0;
    for (int i = 0; i < count; i++) {
        libusb_device *device = original[i];
        if (IsXInputDevice(device) && IdentifyDevice(device, NULL) == RB4_Type_XInputWireless) {
            OIRB4Receiver *receiver = FindReceiver(device);
            if (receiver == NULL)
                receiver = AddReceiver(device);
            if (receiver != NULL) {
                receiver->present = true;
                for (int s = 0; s < RECEIVER_SLOTS; s++) {
                    if (SlotIsVisible(&receiver->slots[s]))
                        replacement[replacement_count++] = SlotDevice(&receiver->slots[s]);
                }
            }
            // receivers are never shown to the game, even if we couldn't open them
            continue;
        }
        replacement[replacement_count++] = device;
    }
    replacement[replacement_count] = NULL;
    CleanUpReceivers();
    AssignSlotAddresses(original, count);

    uint64_t elapsed_ms = (sceKernelGetProcessTime() - start) / 1000;
    if (device_list_calls <= 10 || device_list_calls % 500 == 0 || elapsed_ms > 50)
        final_printf("Device list #%d: %d real -> %d shown, %lu ms\n", device_list_calls, count, replacement_count, (unsigned long)elapsed_ms);

    if (!RememberDeviceList(replacement, original)) {
        OILogLimited(5, "Too many device lists at once, showing the real one\n");
        free(replacement);
        return count;
    }
    *list = replacement;
    return replacement_count;
}

int TsceUsbdFreeDeviceList_hook(libusb_device **list, int unref_devices) {
    // libusb's version takes whether to unref the devices too, so pass that along
    int (*free_device_list)(libusb_device **, int) = (int (*)(libusb_device **, int))sceUsbdFreeDeviceList;
    libusb_device **original = ForgetDeviceList(list);
    if (original == NULL)
        return free_device_list(list, unref_devices);
    int r = free_device_list(original, unref_devices);
    free(list);
    return r;
}

int TsceUsbdGetDeviceDescriptor_hook(libusb_device *device, struct libusb_device_descriptor *desc) {
    bool current = false;
    OIRB4ReceiverSlot *slot = SlotFromVirtualDevice(device, &current);
    if (slot != NULL) {
        if (!current || SlotIsGone(slot))
            return SCE_USBD_ERROR_NO_DEVICE;
        int r = sceUsbdGetDeviceDescriptor(slot->receiver->device, desc);
        if (r == 0 && desc != NULL) {
            slot->kind = KindFromSubtype(slot->subtype, RB4_Kind_Drums);
            SetPS3InstrumentIds(desc, slot->kind);
        }
        return r;
    }

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

        // detect wired XInput instruments using device class + interface descriptor
        // so third party ones should work just fine
        if (desc->bDeviceClass == 0xFF && desc->bDeviceSubClass == 0xFF && desc->bDeviceProtocol == 0xFF) {
            switch (IdentifyDevice(device, NULL)) {
                case RB4_Type_XInputGuitar:
                    SetPS3InstrumentIds(desc, RB4_Kind_Guitar);
                    break;
                case RB4_Type_XInputDrums:
                    SetPS3InstrumentIds(desc, RB4_Kind_Drums);
                    break;
                default:
                    break;
            }
        }
    }
    return r;
}

int TsceUsbdGetConfigDescriptor_hook(libusb_device *device, uint8_t config_index, struct libusb_config_descriptor **config) {
    bool current = false;
    OIRB4ReceiverSlot *slot = SlotFromVirtualDevice(device, &current);
    if (slot != NULL) {
        if (!current || SlotIsGone(slot))
            return SCE_USBD_ERROR_NO_DEVICE;
        // the slot's own endpoints get swapped in when the game sets up its transfers
        device = slot->receiver->device;
    }
    int r = sceUsbdGetConfigDescriptor(device, config_index, config);
    // always set device class to HID - rb4 needs this to actually connect to the device
    if (r == 0 && config != NULL && *config != NULL)
        (*config)->interface->altsetting[0].bInterfaceClass = 0x03;
    return r;
}

uint8_t TsceUsbdGetBusNumber_hook(libusb_device *device) {
    OIRB4ReceiverSlot *slot = SlotFromVirtualDevice(device, NULL);
    if (slot != NULL)
        return slot->receiver->device != NULL ? sceUsbdGetBusNumber(slot->receiver->device) : 0;
    return sceUsbdGetBusNumber(device);
}

uint8_t TsceUsbdGetDeviceAddress_hook(libusb_device *device) {
    OIRB4ReceiverSlot *slot = SlotFromVirtualDevice(device, NULL);
    if (slot != NULL)
        return slot->address != 0 ? slot->address : (uint8_t)(124 + slot->index);
    return sceUsbdGetDeviceAddress(device);
}

int TsceUsbdOpen_hook(libusb_device *device, libusb_device_handle **dev_handle) {
    bool current = false;
    OIRB4ReceiverSlot *slot = SlotFromVirtualDevice(device, &current);
    if (slot != NULL) {
        if (!current || SlotIsGone(slot) || dev_handle == NULL) {
            final_printf("Refused to open receiver slot %d (current %d, gone %d)\n", slot->index + 1, current, SlotIsGone(slot));
            return SCE_USBD_ERROR_NO_DEVICE;
        }
        if (slot->open) {
            // same device opened again: hand out the same handle and count it
            slot->open_count++;
            *dev_handle = SlotHandle(slot);
            final_printf("Opened receiver slot %d again (%d open)\n", slot->index + 1, slot->open_count);
            return 0;
        }
        OIRB4OpenDevice *opendevice = AllocOpenDevice(device);
        if (opendevice == NULL) {
            final_printf("Too many open devices, refused receiver slot %d\n", slot->index + 1);
            return SCE_USBD_ERROR_NO_DEVICE;
        }
        opendevice->device_handle = SlotHandle(slot);
        opendevice->real_handle = slot->receiver->handle;
        opendevice->in_endpoint = SlotInEndpoint(slot);
        opendevice->slot = slot;
        opendevice->type = RB4_Type_XInputWireless;
        opendevice->kind = slot->kind;
        opendevice->subtype = slot->subtype;
        slot->open = true;
        slot->open_count = 1;
        *dev_handle = SlotHandle(slot);
        final_printf("Opened receiver slot %d: kind %d, subtype 0x%02X, address %u\n",
            slot->index + 1, slot->kind, slot->subtype, slot->address);
        return 0;
    }

    int r = sceUsbdOpen(device, dev_handle);
    if (r == 0 && dev_handle != NULL && *dev_handle != NULL) {
        uint8_t subtype = 0;
        OIRB4DeviceType type = IdentifyDevice(device, &subtype);
        if (type == RB4_Type_XInputGuitar || type == RB4_Type_XInputDrums) {
            OIRB4OpenDevice *opendevice = AllocOpenDevice(device);
            if (opendevice == NULL)
                return r;
            opendevice->device_handle = *dev_handle;
            opendevice->real_handle = *dev_handle;
            opendevice->type = type;
            opendevice->kind = type == RB4_Type_XInputGuitar ? RB4_Kind_Guitar : RB4_Kind_Drums;
            opendevice->subtype = subtype;
            opendevice->out_endpoint = FindOutEndpoint(device);
            final_printf("Opened wired instrument: kind %d, subtype 0x%02X, out endpoint 0x%02X\n",
                opendevice->kind, opendevice->subtype, opendevice->out_endpoint);
        }
    }
    return r;
}

void TsceUsbdClose_hook(libusb_device_handle *dev_handle) {
    if (dev_handle == NULL)
        return;
    OIRB4OpenDevice *opendevice = GetOpenDeviceFromDeviceHandle(dev_handle);
    OIRB4ReceiverSlot *slot = SlotFromVirtualHandle(dev_handle);
    if (slot != NULL) {
        if (!slot->open)
            return;
        if (--slot->open_count > 0) {
            final_printf("Closed receiver slot %d (%d still open)\n", slot->index + 1, slot->open_count);
            return;
        }
        final_printf("Closed receiver slot %d\n", slot->index + 1);
        // the receiver stays open, it's ours
        slot->open = false;
        FailPendingGameTransfers(slot, LIBUSB_TRANSFER_CANCELLED);
        if (slot->unplugging) {
            // come back as a new device, the right instrument this time
            slot->unplugging = false;
            slot->identity ^= 1;
        }
        StartWatching(slot);
        CleanUpReceivers();
    } else {
        if (opendevice != NULL)
            final_printf("Closed wired instrument\n");
        sceUsbdClose(dev_handle);
    }
    if (opendevice != NULL)
        opendevice->is_open = false;
}

int TsceUsbdCheckConnected_hook(libusb_device_handle *dev_handle) {
    OIRB4ReceiverSlot *slot = SlotFromVirtualHandle(dev_handle);
    if (slot != NULL) {
        if (SlotIsGone(slot))
            return SCE_USBD_ERROR_NO_DEVICE;
        dev_handle = slot->receiver->handle;
    }
    int r = sceUsbdCheckConnected(dev_handle);
    if (slot != NULL)
        OILogLimited(10, "Check connected, slot %d -> %d (0x%08X)\n", slot->index + 1, r, (unsigned int)r);
    return r;
}

int TsceUsbdControlTransfer_hook(libusb_device_handle *dev_handle, uint8_t bmRequestType, uint8_t bRequest,
    uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength, unsigned int timeout) {
    OIRB4ReceiverSlot *slot = SlotFromVirtualHandle(dev_handle);
    OIRB4OpenDevice *device = GetOpenDeviceFromDeviceHandle(dev_handle);
    bool ours = slot != NULL || device != NULL;
    if (slot != NULL && SlotIsGone(slot))
        return SCE_USBD_ERROR_NO_DEVICE;
    // HID class requests to an instrument get answered the way a PS3 instrument would
    if (device != NULL && (bmRequestType & 0x60) == 0x20) {
        int r = HandlePS3ClassRequest(device, bmRequestType, bRequest, wValue, data, wLength);
        OILogLimited(60, "PS3 request %s: type 0x%02X request 0x%02X value 0x%04X length %u -> %d\n",
            slot != NULL ? "slot" : "wired", bmRequestType, bRequest, wValue, wLength, r);
        return r;
    }
    if (slot != NULL) {
        if ((bmRequestType & 0x80) == 0) {
            // requests to the device (PS3 LEDs and the like) mean nothing to a 360 instrument,
            // and the receiver would pass them on to whatever is in slot 1, so just accept them
            OILogLimited(60, "Control transfer slot %d accepted: type 0x%02X request 0x%02X value 0x%04X index 0x%04X length %u\n",
                slot->index + 1, bmRequestType, bRequest, wValue, wIndex, wLength);
            return wLength;
        }
        // reads go to the receiver, but never get to block the game for long
        dev_handle = slot->receiver->handle;
        if (timeout == 0 || timeout > 500)
            timeout = 500;
    }
    uint64_t start = sceKernelGetProcessTime();
    int r = sceUsbdControlTransfer(dev_handle, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout);
    if (ours)
        OILogLimited(60, "Control transfer %s: type 0x%02X request 0x%02X value 0x%04X index 0x%04X length %u timeout %u -> %d (0x%08X) in %lu ms\n",
            slot != NULL ? "slot" : "wired", bmRequestType, bRequest, wValue, wIndex, wLength, timeout, r, (unsigned int)r,
            (unsigned long)((sceKernelGetProcessTime() - start) / 1000));
    return r;
}

// Descriptor reads only need the handle swapped. The rest of the arguments are passed along
// untouched, as libusb and OrbisUsbd.h don't agree on whether the string one takes a language id.
typedef int (*handle_call_fn)(libusb_device_handle *, uint64_t, uint64_t, uint64_t, uint64_t);

int TsceUsbdGetDescriptor_hook(libusb_device_handle *dev_handle, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    OIRB4ReceiverSlot *slot = SlotFromVirtualHandle(dev_handle);
    if (slot != NULL) {
        if (SlotIsGone(slot))
            return SCE_USBD_ERROR_NO_DEVICE;
        dev_handle = slot->receiver->handle;
    }
    int r = ((handle_call_fn)sceUsbdGetDescriptor)(dev_handle, a, b, c, d);
    if (slot != NULL)
        OILogLimited(20, "Get descriptor, slot %d: args 0x%lX 0x%lX -> %d (0x%08X)\n",
            slot->index + 1, (unsigned long)a, (unsigned long)b, r, (unsigned int)r);
    return r;
}

int TsceUsbdGetStringDescriptorAscii_hook(libusb_device_handle *dev_handle, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    OIRB4ReceiverSlot *slot = SlotFromVirtualHandle(dev_handle);
    if (slot != NULL) {
        if (SlotIsGone(slot))
            return SCE_USBD_ERROR_NO_DEVICE;
        dev_handle = slot->receiver->handle;
    }
    int r = ((handle_call_fn)sceUsbdGetStringDescriptorAscii)(dev_handle, a, b, c, d);
    if (slot != NULL)
        OILogLimited(20, "Get string, slot %d: args 0x%lX 0x%lX -> %d (0x%08X)\n",
            slot->index + 1, (unsigned long)a, (unsigned long)b, r, (unsigned int)r);
    return r;
}

void TsceUsbdFillInterruptTransfer_hook(struct libusb_transfer *transfer, libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *buffer, int length, libusb_transfer_cb_fn callback, void *user_data, unsigned int timeout) {
    OIRB4OpenDevice *device = GetOpenDeviceFromDeviceHandle(dev_handle);
    if (device != NULL) {
        OILogLimited(30, "Game transfer set up: %s endpoint 0x%02X, length %d, timeout %u\n",
            device->slot != NULL ? "slot" : "wired", endpoint, length, timeout);
        // every transfer has to come back to the game with its own handle and callback
        RememberGameTransfer(transfer, callback, dev_handle);
        callback = InstrumentTransferCallback;
        if (device->slot != NULL) {
            // filled in for the slot's endpoints, but never submitted to the hardware:
            // the slot's own reader answers it (see SlotPacketArrived)
            dev_handle = device->real_handle;
            endpoint = (endpoint & 0x80) != 0 ? device->in_endpoint : SlotOutEndpoint(device->slot);
        }
    }
    sceUsbdFillInterruptTransfer(transfer, dev_handle, endpoint, buffer, length, callback, user_data, timeout);
}

int TsceUsbdSubmitTransfer_hook(struct libusb_transfer *transfer) {
    OIRB4ReceiverSlot *slot = SlotFromGameTransfer(transfer);
    if (slot != NULL) {
        if (SlotIsGone(slot)) {
            OILogLimited(20, "Refused a transfer for gone receiver slot %d\n", slot->index + 1);
            return SCE_USBD_ERROR_NO_DEVICE;
        }
        // keep the real handle in there, like a transfer in flight would have
        transfer->dev_handle = slot->receiver->handle;
        if ((transfer->endpoint & 0x80) == 0) {
            // nothing to send a PS3 output report to, so it's done right away
            DeferGameTransfer(transfer, LIBUSB_TRANSFER_COMPLETED, transfer->length);
            OILogLimited(10, "Game OUT transfer to slot %d answered\n", slot->index + 1);
            return 0;
        }
        bool queued = false;
        LockGameQueue();
        if (slot->pending_count < MAX_PENDING_GAME_TRANSFERS) {
            slot->pending[slot->pending_count++] = transfer;
            queued = true;
        }
        UnlockGameQueue();
        StartWatching(slot);
        OILogLimited(10, "Game transfer queued on slot %d (%s)\n", slot->index + 1, queued ? "ok" : "queue full");
        return queued ? 0 : SCE_USBD_ERROR_REQUEST_FAILED;
    }
    int r = sceUsbdSubmitTransfer(transfer);
    if (r != 0 && transfer->callback == InstrumentTransferCallback)
        OILogLimited(20, "Game transfer submit failed: endpoint 0x%02X -> %d (0x%08X)\n", transfer->endpoint, r, (unsigned int)r);
    return r;
}

int TsceUsbdCancelTransfer_hook(struct libusb_transfer *transfer) {
    OIRB4ReceiverSlot *slot = SlotFromGameTransfer(transfer);
    if (slot != NULL) {
        // the cancellation comes back to the game on its next event handling, like a real one
        if (RemovePendingGameTransfer(slot, transfer))
            DeferGameTransfer(transfer, LIBUSB_TRANSFER_CANCELLED, 0);
        OILogLimited(20, "Game cancelled a transfer on slot %d\n", slot->index + 1);
        return 0;
    }
    int r = sceUsbdCancelTransfer(transfer);
    if (transfer->callback == InstrumentTransferCallback)
        OILogLimited(20, "Game cancelled a transfer: endpoint 0x%02X -> %d\n", transfer->endpoint, r);
    return r;
}

// the game handles USB events regularly, which is when completions that were held back go out
int TsceUsbdHandleEventsTimeout_hook(uint64_t a, uint64_t b, uint64_t c) {
    int r = ((int (*)(uint64_t, uint64_t, uint64_t))sceUsbdHandleEventsTimeout)(a, b, c);
    CompleteDeferredGameTransfers();
    return r;
}

#define ADDR_OFFSET 0x00400000
void InitUsbdHooks() {
    OILogOpen();
    // make sure we have the USBD module loaded into memory
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_USBD);
    struct proc_info procInfo;
    sys_sdk_proc_info(&procInfo);
    final_printf("Rock Band 4 %s, applying sceUsbd hooks\n", procInfo.version);

    if (strcmp(procInfo.version, "02.21") != 0) {
        final_printf("This plugin is only compatible with version 02.21 of Rock Band 4.\n");
        return;
    }

    // TODO: Figure out why we can't just hook sceUsbd directly
    // right now this is pretty bad form, to hook the game's own stubs
    // less game-agnostic (not that any other games use Usbd) and less version-agnostic
    // (these are the game's PLT stubs for each import, see tools/ps4_imports.py)
    TsceUsbdCancelTransfer = (void*)(procInfo.base_address + 0x01245140);
    TsceUsbdCheckConnected = (void*)(procInfo.base_address + 0x01245150);
    TsceUsbdClose = (void*)(procInfo.base_address + 0x01245160);
    TsceUsbdControlTransfer = (void*)(procInfo.base_address + 0x01245170);
    TsceUsbdFillInterruptTransfer = (void*)(procInfo.base_address + 0x01245190);
    TsceUsbdFreeDeviceList = (void*)(procInfo.base_address + 0x012451b0);
    TsceUsbdGetBusNumber = (void*)(procInfo.base_address + 0x012451d0);
    TsceUsbdGetConfigDescriptor = (void*)(procInfo.base_address + 0x012451e0);
    TsceUsbdGetDescriptor = (void*)(procInfo.base_address + 0x012451f0);
    TsceUsbdGetDeviceAddress = (void*)(procInfo.base_address + 0x01245200);
    TsceUsbdGetDeviceDescriptor = (void*)(procInfo.base_address + 0x01245210);
    TsceUsbdGetDeviceList = (void*)(procInfo.base_address + 0x01245220);
    TsceUsbdGetStringDescriptorAscii = (void*)(procInfo.base_address + 0x01245230);
    TsceUsbdHandleEventsTimeout = (void*)(procInfo.base_address + 0x01245240);
    TsceUsbdOpen = (void*)(procInfo.base_address + 0x01245260);
    TsceUsbdSubmitTransfer = (void*)(procInfo.base_address + 0x01245270);

    // apply all the hooks to the usbd library
    HOOK(TsceUsbdCancelTransfer);
    HOOK(TsceUsbdCheckConnected);
    HOOK(TsceUsbdClose);
    HOOK(TsceUsbdControlTransfer);
    HOOK(TsceUsbdFillInterruptTransfer);
    HOOK(TsceUsbdFreeDeviceList);
    HOOK(TsceUsbdGetBusNumber);
    HOOK(TsceUsbdGetConfigDescriptor);
    HOOK(TsceUsbdGetDescriptor);
    HOOK(TsceUsbdGetDeviceAddress);
    HOOK(TsceUsbdGetDeviceDescriptor);
    HOOK(TsceUsbdGetDeviceList);
    HOOK(TsceUsbdGetStringDescriptorAscii);
    HOOK(TsceUsbdHandleEventsTimeout);
    HOOK(TsceUsbdOpen);
    HOOK(TsceUsbdSubmitTransfer);
}

void DestroyUsbdHooks() {
    // unhook everything just in case
    UNHOOK(TsceUsbdCancelTransfer);
    UNHOOK(TsceUsbdCheckConnected);
    UNHOOK(TsceUsbdClose);
    UNHOOK(TsceUsbdControlTransfer);
    UNHOOK(TsceUsbdFillInterruptTransfer);
    UNHOOK(TsceUsbdFreeDeviceList);
    UNHOOK(TsceUsbdGetBusNumber);
    UNHOOK(TsceUsbdGetConfigDescriptor);
    UNHOOK(TsceUsbdGetDescriptor);
    UNHOOK(TsceUsbdGetDeviceAddress);
    UNHOOK(TsceUsbdGetDeviceDescriptor);
    UNHOOK(TsceUsbdGetDeviceList);
    UNHOOK(TsceUsbdGetStringDescriptorAscii);
    UNHOOK(TsceUsbdHandleEventsTimeout);
    UNHOOK(TsceUsbdOpen);
    UNHOOK(TsceUsbdSubmitTransfer);
}
