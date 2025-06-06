#include <stdint.h>

// button set 1
#define XINPUT_BUTTON_UP    0x01
#define XINPUT_BUTTON_DOWN  0x02
#define XINPUT_BUTTON_LEFT  0x04
#define XINPUT_BUTTON_RIGHT 0x08
#define XINPUT_BUTTON_START 0x10
#define XINPUT_BUTTON_BACK  0x20
#define XINPUT_BUTTON_L3    0x40
#define XINPUT_BUTTON_R3    0x80

// button set 2
#define XINPUT_BUTTON_LB    0x01
#define XINPUT_BUTTON_RB    0x02
#define XINPUT_BUTTON_GUIDE 0x04
#define XINPUT_BUTTON_A     0x10
#define XINPUT_BUTTON_B     0x20
#define XINPUT_BUTTON_X     0x40
#define XINPUT_BUTTON_Y     0x80

// controller subtypes
#define XINPUT_SUBTYPE_GAMEPAD          0x01
#define XINPUT_SUBTYPE_WHEEL            0x02
#define XINPUT_SUBTYPE_ARCADE_STICK     0x03
#define XINPUT_SUBTYPE_FLIGHT_STICK     0x04
#define XINPUT_SUBTYPE_DANCEPAD         0x05
#define XINPUT_SUBTYPE_GUITAR           0x06
#define XINPUT_SUBTYPE_GUITAR_ALTERNATE 0x07
#define XINPUT_SUBTYPE_DRUM_KIT         0x08
#define XINPUT_SUBTYPE_GUITAR_BASS      0x0B
#define XINPUT_SUBTYPE_ARCADE_PAD       0x13

// XBOX 360 controller LED animations
#define X360_LED_ALL_OFF    0x00
#define X360_LED_ALL_BLINK  0x01
#define X360_LED_P1_FLASH   0x02  // Blink player indicator, then solid on
#define X360_LED_P2_FLASH   0x03
#define X360_LED_P3_FLASH   0x04
#define X360_LED_P4_FLASH   0x05
#define X360_LED_P1         0x06
#define X360_LED_P2         0x07
#define X360_LED_P3         0x08
#define X360_LED_P4         0x09
#define X360_LED_ROTATE     0x0A
#define X360_LED_BLINK      0x0B
#define X360_LED_SLOW_BLINK 0x0C
#define X360_LED_ALTERNATE  0x0D

// reports from the controller all start with this header
typedef struct _xinput_report {
    uint8_t message_type;
    uint8_t message_size;
} __attribute__((packed)) xinput_report;

// report containing controller state
typedef struct _xinput_report_controls {
    xinput_report header;
    uint8_t buttons1;
    uint8_t buttons2;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_stick_x;
    int16_t left_stick_y;
    int16_t right_stick_x;
    int16_t right_stick_y;
    uint8_t unused[6];
} __attribute__((packed)) xinput_report_controls;
