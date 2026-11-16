/* Device database types. Data lives in devices_gen.c (generated). */
#ifndef TDX_DEVICES_H
#define TDX_DEVICES_H

#include <stddef.h>
#include <stdint.h>

enum tdx_class {
	TDX_CLASS_UNKNOWN = 0,
	TDX_CLASS_3DMOUSE,
	TDX_CLASS_2DMOUSE,
	TDX_CLASS_KEYBOARD,
	TDX_CLASS_RECEIVER,
};

enum tdx_lcd_kind {
	TDX_LCD_NONE = 0,
	TDX_LCD_QVGA, /* 320x240 RGB565 via vendor USB interface (SpacePilot Pro) */
};

enum tdx_button_kind {
	TDX_BTN_MASK = 0,  /* report bytes[1..] as LE bitmask, test against value */
	TDX_BTN_NUMBER,    /* report 0x1C: list of pressed u16 button numbers   */
	TDX_BTN_LONGPRESS, /* report 0x1D: LE u32 bitmask lp_mask, button value  */
};

struct tdx_button_def {
	uint8_t report_id;
	uint8_t kind;
	uint32_t value;   /* mask (TDX_BTN_MASK) or button number */
	uint32_t lp_mask; /* long press mask (TDX_BTN_LONGPRESS)  */
	int led;          /* LED index or -1                      */
	const char *button_id;
	const char *v3dk;  /* 3Dconnexion virtual key id, e.g. V3DK_TOP */
	const char *label; /* string resource id, e.g. STR_T           */
};

struct tdx_device_def {
	uint16_t vid, pid;
	const char *id;
	const char *name;
	uint8_t dev_class;
	uint8_t lcd;
	uint8_t hid_iface; /* USB interface number of the HID collection */
	const struct tdx_button_def *buttons;
	uint16_t nbuttons; /* 0 => generic numbering, bit n -> button n */
};

extern const struct tdx_device_def tdx_devices[];
extern const size_t tdx_device_count;

const struct tdx_device_def *tdx_device_lookup(uint16_t vid, uint16_t pid);

#endif
