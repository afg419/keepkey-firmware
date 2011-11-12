/*
 * This file is part of the libopencm3 project.
 *
 * Copyright (C) 2010 Thomas Otto <tommi@viadmin.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "./dogm128.h"

u8 dogm128_ram[1024];
u8 dogm128_cursor_x;
u8 dogm128_cursor_y;

void dogm128_send_command(u8 command)
{
	u32 i;

	gpio_clear(DOGM128_A0_PORT, DOGM128_A0_PIN); /* A0 low for commands */
	spi_send(DOGM128_SPI, command);

	for (i = 0; i <= 500; i++) /* Wait a bit. */
		;
}

void dogm128_send_data(u8 data)
{
	u32 i;

	gpio_set(DOGM128_A0_PORT, DOGM128_A0_PIN); /* A0 high for data */
	spi_send(DOGM128_SPI, data);

	for (i = 0; i <= 500; i++) /* Wait a bit. */
		;
}

void dogm128_init(void)
{
	u32 i;

	/* Reset the display (reset low for dogm128). */
	gpio_clear(DOGM128_RESET_PORT, DOGM128_RESET_PIN);
	for (i = 0; i <= 60000; i++) /* Wait a bit. */
		;

	/* Get the display out of reset (reset high for dogm128). */
	gpio_set(DOGM128_RESET_PORT, DOGM128_RESET_PIN);
	for (i = 0; i <= 60000; i++) /* Wait a bit. */
		;

	gpio_clear(DOGM128_A0_PORT, DOGM128_A0_PIN); /* A0 low for init */

	/* Tell the display that we want to start. */
	spi_set_nss_low(DOGM128_SPI);

	/* Init sequence. */
	dogm128_send_command(DOGM128_DISPLAY_START_ADDRESS_BASE + 0);
	dogm128_send_command(DOGM128_ADC_REVERSE);
	dogm128_send_command(DOGM128_COM_OUTPUT_SCAN_NORMAL);
	dogm128_send_command(DOGM128_DISPLAY_NORMAL);
	dogm128_send_command(DOGM128_BIAS_19);
	dogm128_send_command(DOGM128_POWER_CONTROL_BASE + 0x07);
	dogm128_send_command(DOGM128_BOOSTER_RATIO_SET);
	dogm128_send_command(0x00); /* Booster x4 */
	dogm128_send_command(DOGM128_V0_OUTPUT_RESISTOR_BASE + 0x07);
	dogm128_send_command(DOGM128_ELECTRONIC_VOLUME_MODE_SET);
	dogm128_send_command(0x16); /* Contrast */
	dogm128_send_command(DOGM128_STATIC_INDICATOR_OFF);
	dogm128_send_command(0x00); /* Flashing OFF */
	dogm128_send_command(DOGM128_DISPLAY_ON);

	/* End transfer. */
	spi_set_nss_high(DOGM128_SPI);
}

void dogm128_print_char(u8 data)
{
	u8 i, page, shift, xcoord, ycoord;

	xcoord = dogm128_cursor_x;
	ycoord = dogm128_cursor_y;

	/* The display consists of 8 lines a 8 dots each. */
	page = (63 - ycoord) / 8;
	shift = (7 -((63 - ycoord) % 8)); /* vertical shift */

	/* Font is 8x5 so iterate each column of the character. */
	for (i = 0; i <= 5; i++) {
		/* Right border reached? */
		if ((xcoord + i) > 127)
			return;
		dogm128_cursor_x++;

		/* 0xAA = end of character - no dots in this line. */
		if (dogm128_font[data - 0x20][i] == 0xAA) {
			dogm128_ram[(page * 128) + xcoord + i] &=
				~(0xFF >> shift); /* Clear area. */
			if ((shift > 0) && (page > 0))
				dogm128_ram[((page - 1) * 128) + xcoord + i]
				  &= ~(0xFF << (8 - shift)); /* Clear area. */
			return;
		}

		/* Lower part. */
		dogm128_ram[(page * 128) + xcoord + i] &=
			~(0xFF >> shift); /* Clear area. */
		dogm128_ram[(page * 128) + xcoord + i] =
			(dogm128_font[data - 0x20][i] >> shift);

		/* Higher part if needed. */
		if ((shift > 0) && (page > 0)) {
			dogm128_ram[((page - 1) * 128) + xcoord + i] &=
				~(0xFF << (8 - shift)); /* Clear area. */
			dogm128_ram[((page - 1) * 128) + xcoord + i] =
				(dogm128_font[data - 0x20][i] << (8 - shift));
		}
	}
}

void dogm128_set_cursor(u8 xcoord, u8 ycoord)
{
	dogm128_cursor_x = xcoord;
	dogm128_cursor_y = ycoord;
}

void dogm128_print_string(char *s)
{
	while (*s != 0) {
		dogm128_print_char(*s);
		s++;
	}
}

void dogm128_set_dot(u8 xcoord, u8 ycoord)
{
	dogm128_ram[(((63 - ycoord) / 8) * 128) + xcoord] |=
		(1 << ((63 - ycoord) % 8));
}

void dogm128_clear_dot(u8 xcoord, u8 ycoord)
{
	dogm128_ram[(((63 - ycoord) / 8) * 128) + xcoord] &=
		~(1 << ((63 - ycoord) % 8));
}

void dogm128_update_display(void)
{
	u8 page, column;

	/* Tell the display that we want to start. */
        spi_set_nss_low(DOGM128_SPI);

	for (page = 0; page <= 7; page++) {
		dogm128_send_command(0xB0 + page); /* Set page. */
		dogm128_send_command(0x10); /* Set column upper address to 0. */
		dogm128_send_command(0x00); /* Set column lower address to 0. */

		for (column = 0; column <= 127; column++)
			dogm128_send_data(dogm128_ram[(page * 128) + column]);
	}

	spi_set_nss_high(DOGM128_SPI);
}

void dogm128_clear(void)
{
	int i;

	for (i = 0; i <= 1023; i++)
		dogm128_ram[i] = 0;

	dogm128_update_display();
}

/*
 * This is a non-monospace font definition (upside down for better handling).
 * 0xAA is the end of the character so it's not space efficient in your memory,
 * but on your display.
 *
 * We are starting with " " as the first printable character at 0x20, so we
 * have to substract 0x20 later.
 *
 * Its the only defined to 127/0x7F so if you have German umlauts or other
 * special characters from above you have to expand this definition a
 * little bit.
 */

const u8 dogm128_font[96][6] = {

  /* 20 SPACE  */  {0x00, 0x00, 0x00, 0xAA, 0xAA, 0xAA},
  /* 21 ! */  {0x5E, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 22 " */  {0x66, 0x00, 0x66, 0xAA, 0xAA, 0xAA},
  /* 23 # */  {0x28, 0x7C, 0x28, 0x7C, 0x28, 0xAA},
  /* 24 $ */  {0x24, 0x2A, 0x7F, 0x2A, 0x10, 0xAA},
  /* 25 % */  {0x62, 0x18, 0x46, 0xAA, 0xAA, 0xAA},
  /* 26 & */  {0x30, 0x4C, 0x5A, 0x24, 0x50, 0xAA},
  /* 27 ' */  {0x06, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 28 ( */  {0x3E, 0x41, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 29 ) */  {0x41, 0x3E, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 2A * */  {0x28, 0x10, 0x7C, 0x10, 0x28, 0xAA},
  /* 2B + */  {0x10, 0x38, 0x10, 0xAA, 0xAA, 0xAA},
  /* 2C , */  {0xC0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 2D - */  {0x10, 0x10, 0x10, 0xAA, 0xAA, 0xAA},
  /* 2E . */  {0x40, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 2F / */  {0x60, 0x18, 0x06, 0xAA, 0xAA, 0xAA},

  /* 30 0 */  {0x3C, 0x42, 0x42, 0x3C, 0xAA, 0xAA},
  /* 31 1 */  {0x44, 0x7E, 0x40, 0xAA, 0xAA, 0xAA},
  /* 32 2 */  {0x44, 0x62, 0x52, 0x4C, 0xAA, 0xAA},
  /* 33 3 */  {0x4A, 0x4A, 0x34, 0xAA, 0xAA, 0xAA},
  /* 34 4 */  {0x1E, 0x10, 0x78, 0x10, 0xAA, 0xAA},
  /* 35 5 */  {0x4E, 0x4A, 0x32, 0xAA, 0xAA, 0xAA},
  /* 36 6 */  {0x3C, 0x4A, 0x4A, 0x30, 0xAA, 0xAA},
  /* 37 7 */  {0x62, 0x12, 0x0E, 0xAA, 0xAA, 0xAA},
  /* 38 8 */  {0x34, 0x4A, 0x4A, 0x34, 0xAA, 0xAA},
  /* 39 9 */  {0x0C, 0x52, 0x52, 0x3C, 0xAA, 0xAA},
  /* 3A : */  {0x28, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 3B ; */  {0xC8, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 3C < */  {0x10, 0x28, 0x44, 0xAA, 0xAA, 0xAA},
  /* 3D = */  {0x28, 0x28, 0x28, 0xAA, 0xAA, 0xAA},
  /* 3E > */  {0x44, 0x28, 0x10, 0xAA, 0xAA, 0xAA},
  /* 3F ? */  {0x02, 0x52, 0x0C, 0xAA, 0xAA, 0xAA},

  /* 40 @ */  {0x3C, 0x42, 0x12, 0x2A, 0x3C, 0xAA},
  /* 41 A */  {0x7C, 0x12, 0x12, 0x7C, 0xAA, 0xAA},
  /* 42 B */  {0x7E, 0x4A, 0x4A, 0x34, 0xAA, 0xAA},
  /* 43 C */  {0x3C, 0x42, 0x42, 0x24, 0xAA, 0xAA},
  /* 44 D */  {0x7E, 0x42, 0x42, 0x3C, 0xAA, 0xAA},
  /* 45 E */  {0x7E, 0x4A, 0x4A, 0xAA, 0xAA, 0xAA},
  /* 46 F */  {0x7E, 0x0A, 0x0A, 0xAA, 0xAA, 0xAA},
  /* 47 G */  {0x3C, 0x42, 0x52, 0x34, 0xAA, 0xAA},
  /* 48 H */  {0x7E, 0x08, 0x08, 0x7E, 0xAA, 0xAA},
  /* 49 I */  {0x42, 0x7E, 0x42, 0xAA, 0xAA, 0xAA},
  /* 4A J */  {0x42, 0x42, 0x3E, 0xAA, 0xAA, 0xAA},
  /* 4B K */  {0x7E, 0x08, 0x14, 0x62, 0xAA, 0xAA},
  /* 4C L */  {0x7E, 0x40, 0x40, 0xAA, 0xAA, 0xAA},
  /* 4D M */  {0x7E, 0x04, 0x08, 0x04, 0x7E, 0xAA},
  /* 4E N */  {0x7E, 0x04, 0x18, 0x20, 0x7E, 0xAA},
  /* 4F O */  {0x3C, 0x42, 0x42, 0x3C, 0xAA, 0xAA},

  /* 50 P */  {0x7E, 0x12, 0x12, 0x0C, 0xAA, 0xAA},
  /* 51 Q */  {0x3C, 0x42, 0x42, 0xBC, 0xAA, 0xAA},
  /* 52 R */  {0x7E, 0x12, 0x12, 0x6C, 0xAA, 0xAA},
  /* 53 S */  {0x44, 0x4A, 0x4A, 0x30, 0xAA, 0xAA},
  /* 54 T */  {0x02, 0x7E, 0x02, 0xAA, 0xAA, 0xAA},
  /* 55 U */  {0x3E, 0x40, 0x40, 0x3E, 0xAA, 0xAA},
  /* 56 V */  {0x06, 0x18, 0x60, 0x18, 0x06, 0xAA},
  /* 57 W */  {0x3E, 0x40, 0x3E, 0x40, 0x3E, 0xAA},
  /* 58 X */  {0x42, 0x24, 0x18, 0x24, 0x42, 0xAA},
  /* 59 Y */  {0x9E, 0xA0, 0xA0, 0x7E, 0xAA, 0xAA},
  /* 5A Z */  {0x62, 0x52, 0x4A, 0x46, 0xAA, 0xAA},
  /* 5B [ */  {0x7E, 0x42, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 5C \ */  {0x06, 0x18, 0x60, 0xAA, 0xAA, 0xAA},
  /* 5D ] */  {0x42, 0x7E, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 5E ^ */  {0x04, 0x02, 0x04, 0xAA, 0xAA, 0xAA},
  /* 5F _ */  {0x40, 0x40, 0x40, 0xAA, 0xAA, 0xAA},

  /* 60 ` */  {0x02, 0x04, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 61 a */  {0x20, 0x54, 0x54, 0x78, 0xAA, 0xAA},
  /* 62 b */  {0x7E, 0x44, 0x44, 0x38, 0xAA, 0xAA},
  /* 63 c */  {0x38, 0x44, 0x44, 0x28, 0xAA, 0xAA},
  /* 64 d */  {0x38, 0x44, 0x44, 0x7E, 0xAA, 0xAA},
  /* 65 e */  {0x38, 0x54, 0x54, 0x58, 0xAA, 0xAA},
  /* 66 f */  {0x7C, 0x0A, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 67 g */  {0x98, 0xA4, 0xA4, 0x7C, 0xAA, 0xAA},
  /* 68 h */  {0x7E, 0x04, 0x04, 0x78, 0xAA, 0xAA},
  /* 69 i */  {0x7A, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 6A j */  {0x40, 0x3A, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 6B k */  {0x7E, 0x10, 0x28, 0x44, 0xAA, 0xAA},
  /* 6C l */  {0x7E, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 6D m */  {0x7C, 0x04, 0x78, 0x04, 0x78, 0xAA},
  /* 6E n */  {0x7C, 0x04, 0x04, 0x78, 0xAA, 0xAA},
  /* 6F o */  {0x38, 0x44, 0x44, 0x38, 0xAA, 0xAA},

  /* 70 p */  {0xFC, 0x24, 0x24, 0x18, 0xAA, 0xAA},
  /* 71 q */  {0x18, 0x24, 0x24, 0xFC, 0xAA, 0xAA},
  /* 72 r */  {0x78, 0x04, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 73 s */  {0x48, 0x54, 0x54, 0x20, 0xAA, 0xAA},
  /* 74 t */  {0x04, 0x3E, 0x44, 0xAA, 0xAA, 0xAA},
  /* 75 u */  {0x3C, 0x40, 0x40, 0x3C, 0xAA, 0xAA},
  /* 76 v */  {0x0C, 0x30, 0x40, 0x30, 0x0C, 0xAA},
  /* 77 w */  {0x3C, 0x40, 0x3C, 0x40, 0x3C, 0xAA},
  /* 78 x */  {0x44, 0x28, 0x10, 0x28, 0x44, 0xAA},
  /* 79 y */  {0x1C, 0xA0, 0xA0, 0x7C, 0xAA, 0xAA},
  /* 7A z */  {0x64, 0x54, 0x4C, 0xAA, 0xAA, 0xAA},
  /* 7B { */  {0x08, 0x36, 0x41, 0xAA, 0xAA, 0xAA},
  /* 7C | */  {0x7E, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
  /* 7D } */  {0x41, 0x36, 0x08, 0xAA, 0xAA, 0xAA},
  /* 7E ~ */  {0x20, 0x10, 0x20, 0x10, 0xAA, 0xAA},
  /* 7F DEL  */  {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA},
};
