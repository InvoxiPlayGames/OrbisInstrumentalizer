#pragma once 

#include <stdint.h>

#define ORBIS_PAD_PORT_TYPE_STANDARD 0
#define ORBIS_PAD_PORT_TYPE_SPECIAL  2

#define ORBIS_PAD_DEVICE_CLASS_PAD    0
#define ORBIS_PAD_DEVICE_CLASS_GUITAR 1
#define ORBIS_PAD_DEVICE_CLASS_DRUMS  2

#define ORBIS_PAD_CONNECTION_TYPE_STANDARD 0
#define ORBIS_PAD_CONNECTION_TYPE_REMOTE   2

#define ORBIS_PAD_BUTTON_L3           0x0002
#define ORBIS_PAD_BUTTON_R3           0x0004
#define ORBIS_PAD_BUTTON_OPTIONS      0x0008
#define ORBIS_PAD_BUTTON_UP           0x0010
#define ORBIS_PAD_BUTTON_RIGHT        0x0020
#define ORBIS_PAD_BUTTON_DOWN         0x0040
#define ORBIS_PAD_BUTTON_LEFT         0x0080

#define ORBIS_PAD_BUTTON_L2           0x0100
#define ORBIS_PAD_BUTTON_R2           0x0200
#define ORBIS_PAD_BUTTON_L1           0x0400
#define ORBIS_PAD_BUTTON_R1           0x0800

#define ORBIS_PAD_BUTTON_TRIANGLE     0x1000
#define ORBIS_PAD_BUTTON_CIRCLE       0x2000
#define ORBIS_PAD_BUTTON_CROSS        0x4000
#define ORBIS_PAD_BUTTON_SQUARE       0x8000

#define ORBIS_PAD_BUTTON_TOUCH_PAD    0x100000

#define ORBIS_PAD_MAX_TOUCH_NUM       2
#define ORBIS_PAD_MAX_DATA_NUM        0x40

typedef struct vec_float3
{
    float x;
    float y;
    float z;
} vec_float3;

typedef struct vec_float4
{
    float x;
    float y;
    float z;
    float w;
} vec_float4;

typedef struct stick
{
	uint8_t x;
	uint8_t y;
} stick;

typedef struct analog
{
	uint8_t l2;
	uint8_t r2;
} analog;

typedef struct OrbisPadTouch {
	uint16_t x, y;
	uint8_t finger;
	uint8_t pad[3];
} OrbisPadTouch;

typedef struct OrbisPadTouchData {
	uint8_t fingers;
	uint8_t pad1[3];
	uint32_t pad2;
	OrbisPadTouch touch[ORBIS_PAD_MAX_TOUCH_NUM];
} OrbisPadTouchData;

// The ScePadData Structure contains data polled from the DS4 controller. This includes button states, analogue
// positional data, and touchpad related data.
typedef struct OrbisPadData {
	unsigned int buttons;
	stick leftStick;
	stick rightStick;
	analog analogButtons;
	uint16_t padding;
	vec_float4 quat;
	vec_float3 vel;
	vec_float3 acell;
	OrbisPadTouchData touch;
	uint8_t connected;
	uint64_t timestamp;
	uint8_t ext[16];
	uint8_t count;
	uint8_t unknown[15];
} OrbisPadData;

// The PadColor structure contains RGBA for the DS4 controller lightbar.
typedef struct OrbisPadColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
} OrbisPadColor;

typedef struct OrbisPadVibeParam {
	uint8_t lgMotor;
	uint8_t smMotor;
} OrbisPadVibeParam;

// Vendor information about which controller to open for scePadOpenExt
typedef struct _OrbisPadExtParam {
    uint16_t vendorId;
    uint16_t productId;
    uint16_t productId_2; // this is in here twice?
    uint8_t unknown[10];
} OrbisPadExtParam;

typedef struct _OrbisPadInformation {
	float touchpadDensity;
	uint16_t touchResolutionX;
	uint16_t touchResolutionY;
	uint8_t stickDeadzoneL;
	uint8_t stickDeadzoneR;
	uint8_t connectionType;
	uint8_t count;
	int connected;
	int deviceClass;
	uint8_t unknown[8];
} OrbisPadInformation;
