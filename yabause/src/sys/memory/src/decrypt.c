/* Encryption/decryption 315-5881 chip support
 * used on some ST-V PCB
 * Based on mame 315-5881_crypt.cpp and stvprot.cpp
 * // license:BSD-3-Clause
 * // copyright-holders:Andreas Naive, Olivier Galibert, David Haywood
 */

#include <stdlib.h>
#include <string.h>
#include "memory.h"
#include "cs0.h"

typedef struct sbox_s {
  u8 table[64];
  s8 inputs[6];      // positions of the inputs bits, -1 means no input except from key
  u8 outputs[2];     // positions of the output bits
} sbox;

#define FN1GK 38
#define FN2GK 32

#define BUFFER_SIZE 2
#define LINE_SIZE 512
#define FLAG_COMPRESSED 0x20000

#define BIT(A, B) ((A >> B)&0X1)

static const sbox fn1_sboxes[4][4] = {
	{   // 1st round
		{
			{
				0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,
				2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3,
			},
			{3,4,5,7,-1,-1},
			{0,4}
		},

		{
			{
				2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,
				0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2,
			},
			{0,1,2,5,6,7},
			{1,6}
		},

		{
			{
				0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,
				1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3,
			},
			{0,2,5,6,7,-1},
			{2,3}
		},

		{
			{
				3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,
				1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3,
			},
			{1,2,3,4,6,7},
			{5,7}
		},
	},
	{   // 2nd round
		{
			{
				3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,
				2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3,
			},
			{0,1,3,4,5,7},
			{0,4}
		},

		{
			{
				2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,
				3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1,
			},
			{0,1,3,4,6,-1},
			{1,5}
		},

		{
			{
				2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,
				3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2,
			},
			{1,2,5,6,7,6},
			{2,7}
		},

		{
			{
				0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,
				0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0,
			},
			{2,3,4,5,6,7},
			{3,6}
		},
	},
	{   // 3rd round
		{
			{
				0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,
				0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1,
			},
			{1,2,3,4,5,7},
			{0,5}
		},

		{
			{
				1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,
				1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1,
			},
			{0,2,4,5,6,7},
			{1,6}
		},

		{
			{
				0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,
				// unused?
				255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
			},
			{0,2,4,6,7,-1},
			{2,3}
		},

		{
			{
				0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,
				2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3,
			},
			{0,1,2,3,6,-1},
			{4,7}
		},
	},
	{   // 4th round
		{
			{
				0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,
				3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1,
			},
			{0,1,3,4,5,6},
			{0,5}
		},

		{
			{
				0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,
				3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3,
			},
			{0,1,2,3,5,7},
			{1,7}
		},

		{
			{
				0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,
				3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0,
			},
			{2,3,4,5,6,7},
			{2,3}
		},

		{
			{
				0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,
				1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1,
			},
			{0,1,2,4,6,7},
			{4,6}
		},
	},
};


static const sbox fn2_sboxes[4][4] = {
	{   // 1st round
		{
			{
				3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,
				2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2,
			},
			{1,3,4,5,6,7},
			{0,7}
		},

		{
			{
				0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,
				3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0,
			},
			{0,3,5,6,5,0},
			{1,2}
		},

		{
			{
				0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,
				0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2,
			},
			{0,2,3,4,7,-1},
			{3,4}
		},

		{
			{
				2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,
				2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1,
			},
			{1,2,5,6,-1,-1},
			{5,6}
		},
	},
	{   // 2nd round
		{
			{
				2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,
				2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0,
			},
			{0,1,4,5,6,7},
			{0,7}
		},

		{
			{
				0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,
				1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3,
			},
			{0,2,3,6,7,-1},
			{1,5}
		},

		{
			{
				1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,
				1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3,
			},
			{2,3,4,6,7,2},
			{2,3}
		},

		{
			{
				2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,
				3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0,
			},
			{1,2,3,4,5,6},
			{4,6}
		},
	},
	{   // 3rd round
		{
			{
				0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,
				3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0,
			},
			{1,4,5,6,7,5},
			{0,5}
		},

		{
			{
				0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,
				2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2,
			},
			{0,2,3,4,6,7},
			{1,7}
		},

		{
			{
				2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,
				// unused?
				255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
			},
			{1,2,4,7,2,-1},
			{2,4}
		},

		{
			{
				0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,
				3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3,
			},
			{0,1,2,3,5,7},
			{3,6}
		},
	},
	{   // 4th round
		{
			{
				0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,
				0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1,
			},
			{0,1,2,4,7,-1},
			{0,5}
		},

		{
			{
				2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,
				0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1,
			},
			{0,1,3,5,6,-1},
			{1,3}
		},

		{
			{
				0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,
				3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1,
			},
			{0,2,3,4,5,7},
			{2,7}
		},

		{
			{
				0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,
				2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2,
			},
			{2,3,4,5,6,7},
			{4,6}
		},
	},
};

static const int fn1_game_key_scheduling[FN1GK][2] = {
	{1,29},  {1,71},  {2,4},   {2,54},  {3,8},   {4,56},  {4,73},  {5,11},
	{6,51},  {7,92},  {8,89},  {9,9},   {9,39},  {9,58},  {10,90}, {11,6},
	{12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43},
	{19,82}, {20,81}, {21,32}, {22,5},  {23,66}, {24,13}, {24,45}, {25,12},
	{25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86}
};

static const int fn2_game_key_scheduling[FN2GK][2] = {
	{0,0},   {1,3},   {2,11},  {3,20},  {4,22},  {5,23},  {6,29},  {7,38},
	{8,39},  {9,55},  {9,86},  {9,87},  {10,50}, {11,57}, {12,59}, {13,61},
	{14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17},
	{22,6},  {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90}
};

static const int fn1_sequence_key_scheduling[20][2] = {
	{0,52},  {1,34},  {2,17},  {3,36}, {4,84},  {4,88},  {5,57},  {6,48},
	{6,68},  {7,76},  {8,83},  {9,30}, {10,22}, {10,41}, {11,38}, {12,55},
	{13,74}, {14,19}, {14,80}, {15,26}
};

static const u8 trees[9][2][32] = {
	{
		{0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,
			0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,
			0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,
			0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff,},
		{0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,
			0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff,},
	},
	{
		{0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,
			0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,
			0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,
			0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,
			0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,
			0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,
			0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
	{
		{0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
		{0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
			0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
	},
};


static const int fn2_sequence_key_scheduling[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};

static const int fn2_middle_result_scheduling[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

static int feistel_function(int input, const sbox *sboxes, uint32_t subkeys)
{
	int k,m;
	int aux;
	int result=0;

	for (m=0; m<4; ++m) { // 4 sboxes
		for (k=0, aux=0; k<6; ++k)
			if (sboxes[m].inputs[k]!=-1)
				aux |= BIT(input, sboxes[m].inputs[k]) << k;

		aux = sboxes[m].table[(aux^subkeys)&0x3f];

		for (k=0; k<2; ++k)
			result |= BIT(aux,k) << sboxes[m].outputs[k];

		subkeys >>=6;
	}

	return result;
}

/**************************
This implementation is an "educational" version. It must be noted that it can be speed-optimized in a number of ways.
The most evident one is to factor out the parts of the key-scheduling that must only be done once (like the game-key &
sequence-key parts) as noted in the comments inlined in the function. More sophisticated speed-ups can be gained by
noticing that the weak key-scheduling would allow to create some pregenerated look-up tables for doing most of the work
of the function. Even so, it would still be pretty slow, so caching techniques could be a wiser option here.
**************************/

static u32 cryptoKey = 0;
static u32 cryptoSubKey = 0;
static u32 cryptoAddr = 0;
static u8 cryptoReady = 0;
static int bufferBit = 0;
static int bufferBit2 = 0;

static u16 dec_hist = 0;
static u32 dec_header = 0;

static u32 buffer_pos = 0;
static u32 line_buffer_pos = 0;
static u32 line_buffer_size = 0;
static u8 done_compression = 0;
static u32 block_pos = 0;
static u32  block_size = 0;
static u32 block_numlines = 0;
static u16 buffer2a = 0;
static u8 buffer2[2];

static u8 buffer[BUFFER_SIZE];
static u8 line_buffer[LINE_SIZE];
static u8 line_buffer_prev[LINE_SIZE];

void cryptoReset()
{
  cryptoKey = 0;
  cryptoSubKey = 0;
  cryptoAddr = 0;
  cryptoReady = 0;
  bufferBit = 0;
  bufferBit2 = 0;

  memset(buffer, 0, BUFFER_SIZE);
  memset(line_buffer, 0, LINE_SIZE);
  memset(line_buffer_prev, 0, LINE_SIZE);

	dec_hist = 0;
	dec_header = 0;

	buffer_pos = 0;
	line_buffer_pos = 0;
	line_buffer_size = 0;

  /* These were left over from the previous game/session: block_* and
     done_compression are only rewritten by cryptoStart(), buffer2a by
     get_compressed_bit(), and cryptoStart() reads buffer2a when
     bufferBit2 < 14. */
  done_compression = 0;
  block_pos = 0;
  block_size = 0;
  block_numlines = 0;
  buffer2a = 0;
  buffer2[0] = buffer2[1] = 0;
}

void cryptoSetKey(u32 privKey)
{
  cryptoKey = privKey;
}

void cyptoSetLowAddr(u16 val)
{
  cryptoAddr = (cryptoAddr & 0xffff0000) | val;
  cryptoReady = 0;
}

void cyptoSetHighAddr(u16 val)
{
  cryptoAddr = (cryptoAddr & 0x0000ffff) | (val<<16);
  cryptoReady = 0;
  bufferBit = 7;
  bufferBit2 = 15;
}

void cyptoSetSubkey(u16 subKey)
{
  cryptoSubKey = subKey;
  cryptoReady = 0;
}

static u16 bitswap16(u16 in, const u8* vec)
{
  u16 ret = 0;
  for (int i = 0; i<16; i++) {
    ret |= ((in >> vec[i])&0x1)<<(15-i); //Peut etre 16-i...
  }
  return ret;
}

static const u8 vec1[16] = {5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2};
static const u8 vec2[16] = {14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10};
static const u8 vec3[16] = {15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8};

static u16 block_decrypt(uint32_t game_key, uint16_t sequence_key, uint16_t counter, uint16_t data)
{
	int j;
	int aux, aux2;
	int A, B;
	int middle_result;
	u32 fn1_subkeys[4];
	u32 fn2_subkeys[4];

	/* Game-key scheduling; this could be done just once per game at initialization time */
	memset(fn1_subkeys, 0, sizeof(u32) * 4);
	memset(fn2_subkeys, 0, sizeof(u32) * 4);

	for (j = 0; j < FN1GK; ++j) {
		if (BIT(game_key, fn1_game_key_scheduling[j][0]) != 0) {
			aux = fn1_game_key_scheduling[j][1] % 24;
			aux2 = fn1_game_key_scheduling[j][1] / 24;
			fn1_subkeys[aux2] ^= (1 << aux);
		}
	}

	for (j = 0; j < FN2GK; ++j) {
		if (BIT(game_key, fn2_game_key_scheduling[j][0]) != 0) {
			aux = fn2_game_key_scheduling[j][1] % 24;
			aux2 = fn2_game_key_scheduling[j][1] / 24;
			fn2_subkeys[aux2] ^= (1 << aux);
		}
	}
	/********************************************************/

	/* Sequence-key scheduling; this could be done just once per decryption run */
	for (j = 0; j < 20; ++j) {
		if (BIT(sequence_key, fn1_sequence_key_scheduling[j][0]) != 0) {
			aux = fn1_sequence_key_scheduling[j][1] % 24;
			aux2 = fn1_sequence_key_scheduling[j][1] / 24;
			fn1_subkeys[aux2] ^= (1 << aux);
		}
	}

	for (j = 0; j < 16; ++j) {
		if (BIT(sequence_key, j) != 0) {
			aux = fn2_sequence_key_scheduling[j] % 24;
			aux2 = fn2_sequence_key_scheduling[j] / 24;
			fn2_subkeys[aux2] ^= (1 << aux);
		}
	}

	/**************************************************************/

	// First Feistel Network
	aux = bitswap16(counter, vec1);

	// 1st round
	B = aux >> 8;
	A = (aux & 0xff) ^ feistel_function(B, fn1_sboxes[0], fn1_subkeys[0]);

	// 2nd round
	B ^= feistel_function(A, fn1_sboxes[1], fn1_subkeys[1]);

	// 3rd round
	A ^= feistel_function(B, fn1_sboxes[2], fn1_subkeys[2]);

	// 4th round
	B ^= feistel_function(A, fn1_sboxes[3], fn1_subkeys[3]);

	middle_result = (B << 8) | A;


	/* Middle-result-key sheduling */
	for (j = 0; j < 16; ++j) {
		if (BIT(middle_result, j) != 0) {
			aux = fn2_middle_result_scheduling[j] % 24;
			aux2 = fn2_middle_result_scheduling[j] / 24;
			fn2_subkeys[aux2] ^= (1 << aux);
		}
	}
	/*********************/

	// Second Feistel Network

	aux = bitswap16(data, vec2);

	// 1st round
	B = aux >> 8;
	A = (aux & 0xff) ^ feistel_function(B, fn2_sboxes[0], fn2_subkeys[0]);

	// 2nd round
	B ^= feistel_function(A, fn2_sboxes[1], fn2_subkeys[1]);

	// 3rd round
	A ^= feistel_function(B, fn2_sboxes[2], fn2_subkeys[2]);

	// 4th round
	B ^= feistel_function(A, fn2_sboxes[3], fn2_subkeys[3]);

	aux = (B << 8) | A;
	aux = bitswap16(aux, vec3);

	return aux;
}

static u16 m_read(uint32_t addr)
{
	u16 dat= DMAMappedMemoryReadWord(0x02000000+2*addr);
	return ((dat&0xff00)>>8)|((dat&0x00ff)<<8);
}

static u16 get_decrypted_16()
{
  uint16_t enc;

  enc = m_read(cryptoAddr);

  uint16_t dec = block_decrypt(cryptoKey, cryptoSubKey, cryptoAddr, enc);

  uint16_t res = (dec & 3) | (dec_hist & 0xfffc);
  dec_hist = dec;

  cryptoAddr ++;

  return res;
}

static void cryptoStart()
{
  block_pos = 0;
  done_compression = 0;
  buffer_pos = BUFFER_SIZE;

  if (bufferBit2 < 14)
  {
    dec_header = (buffer2a & 0x0003) << 16;
  }
  else
  {
    dec_hist = 0; // seems to be needed by astrass at least otherwise any call after the first one will be influenced by the one before it.
    dec_header = get_decrypted_16() << 16;
  }

  dec_header |= get_decrypted_16();

  block_numlines = ((dec_header & 0x000000ff) >> 0) + 1;
  u32 blocky = ((dec_header & 0x0001ff00) >> 8) + 1;
  block_size = block_numlines * blocky;

  if(dec_header & FLAG_COMPRESSED)
  {
    line_buffer_size = blocky;
    line_buffer_pos = line_buffer_size;
    bufferBit = 7;
    bufferBit2 = 15;
  }
  cryptoReady = 1;
}

static u32 get_compressed_bit()
{
//  if(buffer_pos == BUFFER_SIZE)
//      enc_fill();

	if (bufferBit2 == 15)
	{
		bufferBit2 = 0;
		buffer2a = get_decrypted_16();
		buffer2[0] = buffer2a;
		buffer2[1] = buffer2a >> 8;
	//  block_pos+=2;
		buffer_pos = 0;

	}
	else
	{
		bufferBit2++;
	}

//  if (bufferBit ==7) printf("using byte %02x\n", buffer2[(buffer_pos&1) ^ 1]);

	u32 res = (buffer2[(buffer_pos&1)^1] >> bufferBit) & 1;
	bufferBit--;
	if(bufferBit == -1) {
		bufferBit = 7;
		buffer_pos++;
	}
	return res;
}

static void line_fill()
{
	uint8_t *lc = line_buffer;
	uint8_t *lp = line_buffer_prev;

	memcpy(line_buffer_prev, line_buffer, LINE_SIZE);

	line_buffer_pos = 0;

	for(int i=0; i != line_buffer_size;) {
		// vlc 0: start of line
		// vlc 1: interior of line
		// vlc 2-9: 7-1 bytes from end of line
		int slot = i ? i < line_buffer_size - 7 ? 1 : (i & 7) + 1 : 0;
		uint32_t tmp = 0;
		while (!(tmp&0x80)) {
			if(get_compressed_bit()) {
				tmp = trees[slot][1][tmp];
			} else {
				tmp = trees[slot][0][tmp];
                        }
                }
		if(tmp != 0xff) {
			int count = (tmp & 7) + 1;

			if(tmp&0x40) {
				// Copy from previous line

				static int offsets[4] = {0, 1, 0, -1};
				int offset = offsets[(tmp & 0x18) >> 3];
				for(int j=0; j != count; j++) {
					lc[i^1] = lp[((i+offset) % line_buffer_size)^1];
					i++;
				}
			} else {
				// Get a byte in the stream and write n times
				uint8_t byte;
				byte =         get_compressed_bit()  << 1;
				byte = (byte | get_compressed_bit()) << 1;
				byte = (byte | get_compressed_bit()) << 1;
				byte = (byte | get_compressed_bit()) << 1;
				byte = (byte | get_compressed_bit()) << 1;
				byte = (byte | get_compressed_bit()) << 1;
				byte = (byte | get_compressed_bit()) << 1;
				byte =  byte | get_compressed_bit();
				for(int j=0; j != count; j++)
					lc[(i++)^1] = byte;
			}
		}
	}
	block_pos++;
	if (block_numlines == block_pos)
	{
		done_compression = 1;
	}
	else
	{
	}
}

static void enc_fill()
{
	for(int i = 0; i != BUFFER_SIZE; i+=2) {
		uint16_t val = get_decrypted_16();
		buffer[i] = val;
		buffer[i+1] = val >> 8;
		block_pos+=2;

		if (!(dec_header & FLAG_COMPRESSED))
		{
			if (block_pos == block_size)
			{
				// if we reach the size specified we need to read a new header
				// todo: for compressed blocks this depends on OUTPUT size, not input size, so things get messy

				cryptoStart();
			}
		}
	}
	buffer_pos = 0;
}

u16 cryptoDecrypt()
{
  u8* base;
  if(!cryptoReady)
    cryptoStart();
	if(dec_header & FLAG_COMPRESSED) {
		if (line_buffer_pos == line_buffer_size) // if there's no data left to read..
		{
			if (done_compression == 1)
				cryptoStart();


			line_fill();
		}
		base = line_buffer + line_buffer_pos;
		line_buffer_pos += 2;
	} else {
		if(buffer_pos == BUFFER_SIZE)
			enc_fill();
		base = &buffer[buffer_pos];
		buffer_pos += 2;
	}
	return (base[0] << 8) | base[1];
}

/*============================================================================
 * 315-5838 / 317-0229 / 317-0230 / 317-0231 compression+encryption device.
 * Ported from MAME's 315-5838_317-0229_comp.cpp (David Haywood, Samuel
 * Neves, Peter Wilhelmsen, Morten Shearman Kirkegaard).
 *
 * Each cartridge word goes through a fixed word substitution (four 3-bit
 * lanes through 8-entry S-boxes + four affine output bits), then a
 * programmable prefix decompression whose tree/dictionary the game uploads
 * itself. The substitution is NOT identical on every chip: routing, S-boxes
 * and affine masks depend on the variant (see d5838Ciphers below):
 *   315-5838: DecAthlete
 *   317-0229: Name Club 1/2
 *   317-0230: Print Club Pooh 2/3, Nightmare Before Christmas
 *   317-0231: Print Club Love Love 1/2, Sony Creative 5/6
 *============================================================================*/

typedef struct {
  u8 routing[4];
  u8 sboxes[4][8];
  u8 affine[4];
} d5838Cipher;

static const d5838Cipher d5838Ciphers[4] =
{
  { /* 315-5838 */
    { 0, 1, 2, 3 },
    { { 6, 7, 2, 0, 5, 1, 3, 4 }, { 5, 3, 7, 2, 1, 6, 4, 0 },
      { 2, 0, 6, 1, 3, 7, 4, 5 }, { 6, 5, 7, 4, 1, 3, 0, 2 } },
    { 0x14, 0x18, 0x02, 0x15 }
  },
  { /* 317-0229 */
    { 3, 2, 0, 1 },
    { { 6, 0, 5, 4, 7, 2, 1, 3 }, { 5, 6, 1, 3, 7, 0, 4, 2 },
      { 2, 6, 0, 1, 3, 4, 7, 5 }, { 6, 5, 7, 4, 1, 3, 0, 2 } },
    { 0x12, 0x11, 0x04, 0x1a }
  },
  { /* 317-0230 */
    { 2, 3, 1, 0 },
    { { 6, 2, 7, 0, 5, 3, 1, 4 }, { 5, 3, 7, 2, 1, 6, 4, 0 },
      { 2, 1, 3, 5, 6, 0, 4, 7 }, { 6, 1, 3, 5, 7, 0, 2, 4 } },
    { 0x14, 0x18, 0x02, 0x15 }
  },
  { /* 317-0231 */
    { 1, 0, 3, 2 },
    { { 6, 0, 5, 4, 7, 2, 1, 3 }, { 5, 1, 6, 3, 7, 4, 0, 2 },
      { 2, 1, 3, 5, 6, 0, 4, 7 }, { 6, 3, 1, 5, 7, 2, 0, 4 } },
    { 0x12, 0x11, 0x04, 0x1a }
  }
};

static struct {
  u8  variant;        /* index in d5838Ciphers, set before Reset */
  u32 source;         /* word index inside the selected 8MB bank */
  int abort_;
  u8  bank;           /* 0, 1 or 2 - which 8MB window of cart ROM */
  u16 control;        /* register at 0x4200000: bit 1 set = decoder output,
                         clear = plain ROM reads (Name Club uses both) */
  u16 tree[24];
  u8  dictionary[256];
  int tree_words;
  int dictionary_bytes;
  int upload_dictionary;
  u16 word;
  int bits;
} d5838;

void decathlt5838SetVariant(int variant)
{
  d5838.variant = (u8)((variant >= 0 && variant < 4) ? variant : 0);
}

void decathlt5838SetControl(u16 val)
{
  d5838.control = val;
}

int decathlt5838DecoderEnabled(void)
{
  return (d5838.control & 2) != 0;
}

/* Called on EVERY write into the 24MB protection window (regardless of
 * which sub-register), like MAME's sega5838_w() recomputing m_5838_bank.
 * addr is the CS0-relative address (0-0x1FFFFFF). */
void decathlt5838SetBank(u32 addr)
{
  d5838.bank = (u8)((addr >> 23) & 0x3);
}

void decathlt5838Reset(void)
{
  d5838.source = 0;
  d5838.abort_ = 0;
  d5838.bank = 0;
  d5838.control = 2; /* DecAthlete starts decoding without writing it */
  memset(d5838.tree, 0, sizeof(d5838.tree));
  memset(d5838.dictionary, 0, sizeof(d5838.dictionary));
  d5838.tree_words = 0;
  d5838.dictionary_bytes = 0;
  d5838.upload_dictionary = 0;
  d5838.word = 0;
  d5838.bits = 0;
}

static u16 decathlt5838Decipher(u16 c)
{
  static const u8 OUTPUT_BITS[4][3] = { { 0, 1, 7 }, { 2, 5, 9 }, { 4, 10, 13 }, { 6, 11, 15 } };
  static const u8 AFFINE_BITS[4] = { 3, 8, 12, 14 };
  const d5838Cipher *cipher = &d5838Ciphers[d5838.variant];
#define X5838(b) (((c) >> (b)) & 1)
  u8 lanes[4];
  u8 affine;
  u16 result = 0;
  int group, bit;

  lanes[0] = (u8)((X5838(5) ^ X5838(12)) | ((X5838(7) ^ X5838(9)) << 1) | (X5838(14) << 2));
  lanes[1] = (u8)(X5838(6) | (X5838(8) << 1) | (X5838(15) << 2));
  lanes[2] = (u8)((X5838(1) ^ X5838(8)) | (X5838(7) << 1) | (X5838(12) << 2));
  lanes[3] = (u8)(X5838(3) | (X5838(4) << 1) | (X5838(10) << 2));
  affine = (u8)(X5838(0) | ((X5838(0) ^ X5838(2)) << 1) | ((X5838(11) ^ X5838(14)) << 2) |
                ((X5838(4) ^ X5838(13)) << 3) | 16);
#undef X5838

  for (group = 0; group < 4; group++)
  {
    u8 value = cipher->sboxes[group][lanes[cipher->routing[group]]];
    u8 parity;
    for (bit = 0; bit < 3; bit++)
      result |= ((value >> bit) & 1) << OUTPUT_BITS[group][bit];

    parity = affine & cipher->affine[group];
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;
    result |= (parity & 1) << AFFINE_BITS[group];
  }
  return result;
}

/* Next encrypted word straight from cartridge ROM. Like MAME's
 * sega5838_source_r() the word offset is confined to the selected 8MB bank.
 * T1ReadWord() already yields the correctly swapped 16-bit word. */
static u16 decathlt5838SourceWordRead(void)
{
  u32 address = ((u32)d5838.bank << 23) | ((d5838.source * 2) & 0x007FFFFF);
  return T1ReadWord(CartridgeArea->rom, address & 0x1FFFFFF);
}

static u8 decathlt5838DecompressByte(void)
{
  u16 code = 0;
  unsigned length;

  if (d5838.abort_ || d5838.tree_words != 24 || d5838.dictionary_bytes != 256)
    return 0xFF;

  for (length = 1; length <= 12; length++)
  {
    unsigned entry;

    if (!d5838.bits)
    {
      d5838.word = decathlt5838Decipher(decathlt5838SourceWordRead());
      d5838.source = (d5838.source + 1) & 0x007FFFFF;
      d5838.bits = 16;
    }
    code = (u16)((code << 1) | ((d5838.word >> --d5838.bits) & 1));

    for (entry = 0; entry < 24; entry += 2)
    {
      u16 first, limit;
      unsigned next;

      if ((d5838.tree[entry] >> 8) != length)
        continue;

      first = d5838.tree[entry + 1] >> (12 - length);
      limit = (u16)(1U << length);
      /* The range ends at the next DISTINCT pattern: repeated final
         descriptors must not terminate it (several variants finish before
         length 12). */
      for (next = entry + 2; next < 24; next += 2)
      {
        if (d5838.tree[next + 1] != d5838.tree[entry + 1])
        {
          limit = d5838.tree[next + 1] >> (12 - length);
          break;
        }
      }

      if (code >= first && code < limit)
      {
        unsigned index = (d5838.tree[entry] & 0xFF) + code - first;
        if (index < 256)
          return d5838.dictionary[index];
      }
    }
  }

  /* Invalid tables or data must not loop forever. */
  d5838.abort_ = 1;
  return 0xFF;
}

u16 decathlt5838DataRead(void)
{
  u8 b0 = decathlt5838DecompressByte();
  u8 b1 = decathlt5838DecompressByte();
  return (u16)((b0 << 8) | b1);
}

/* source_w equivalent. mem_mask mirrors MAME's COMBINE_DATA: 0xFFFFFFFF for
 * a 32-bit write, 0xFFFF0000/0x0000FFFF for one half only. */
void decathlt5838SetSrcAddr(u32 val, u32 mem_mask)
{
  d5838.source = (d5838.source & ~mem_mask) | (val & mem_mask);
  d5838.source &= 0x007FFFFF;
  d5838.word = 0;
  d5838.bits = 0;
  d5838.abort_ = 0;
}

/* Upper half of the data register: mode select. Bit 7 set = dictionary
 * upload, clear = tree upload; either restarts the matching cursor. */
void decathlt5838SetTableUploadMode(u16 val)
{
  d5838.upload_dictionary = (val & 0x80) != 0;
  if (d5838.upload_dictionary)
    d5838.dictionary_bytes = 0;
  else
    d5838.tree_words = 0;
}

/* Lower half of the data register: payload, cursor auto-increments. */
void decathlt5838UploadTableData(u16 val)
{
  if (d5838.upload_dictionary)
  {
    if (d5838.dictionary_bytes < 256)
    {
      d5838.dictionary[d5838.dictionary_bytes++] = (u8)(val >> 8);
      d5838.dictionary[d5838.dictionary_bytes++] = (u8)(val & 0xFF);
    }
  }
  else if (d5838.tree_words < 24)
  {
    d5838.tree[d5838.tree_words++] = val;
  }
}

/*============================================================================
 * Save states (CART chunk, ST-V block - see CartSaveState() in cs0.c).
 * Every field is written with a fixed size so the layout does not depend on
 * the compiler's struct padding. The game configuration (cryptoKey from the
 * game list, d5838.variant from the game init) is not saved: it is set again
 * when the game is loaded and the state belongs to that game.
 *============================================================================*/

static void DecStateW8(u8 v, void ** stream)   { MemStateWrite(&v, 1, 1, stream); }
static void DecStateW16(u16 v, void ** stream) { MemStateWrite(&v, 2, 1, stream); }
static void DecStateW32(u32 v, void ** stream) { MemStateWrite(&v, 4, 1, stream); }
static u8  DecStateR8(const void * stream)  { u8 v;  MemStateRead(&v, 1, 1, stream); return v; }
static u16 DecStateR16(const void * stream) { u16 v; MemStateRead(&v, 2, 1, stream); return v; }
static u32 DecStateR32(const void * stream) { u32 v; MemStateRead(&v, 4, 1, stream); return v; }

#define CRYPTO_STATE_VERSION 1
#define CRYPTO_STATE_SIZE (4 + 4*2 + 1 + 4*2 + 2 + 4 + 4*3 + 1 + 4*3 + 2 + 2 + BUFFER_SIZE + 2*LINE_SIZE)

/* 315-5881: position inside the current block, so a state saved in the
   middle of a decompressed transfer resumes on the same word. */
void cryptoSaveState(void ** stream)
{
  DecStateW32(CRYPTO_STATE_VERSION, stream);
  DecStateW32(cryptoSubKey, stream);
  DecStateW32(cryptoAddr, stream);
  DecStateW8(cryptoReady, stream);
  DecStateW32((u32)bufferBit, stream);
  DecStateW32((u32)bufferBit2, stream);
  DecStateW16(dec_hist, stream);
  DecStateW32(dec_header, stream);
  DecStateW32(buffer_pos, stream);
  DecStateW32(line_buffer_pos, stream);
  DecStateW32(line_buffer_size, stream);
  DecStateW8(done_compression, stream);
  DecStateW32(block_pos, stream);
  DecStateW32(block_size, stream);
  DecStateW32(block_numlines, stream);
  DecStateW16(buffer2a, stream);
  MemStateWrite(buffer2, 1, 2, stream);
  MemStateWrite(buffer, 1, BUFFER_SIZE, stream);
  MemStateWrite(line_buffer, 1, LINE_SIZE, stream);
  MemStateWrite(line_buffer_prev, 1, LINE_SIZE, stream);
}

int cryptoLoadState(const void * stream, int size)
{
  u32 version;
  int bb, bb2;

  if (size < CRYPTO_STATE_SIZE)
    return -1;
  version = DecStateR32(stream);
  if (version != CRYPTO_STATE_VERSION)
    return -1;

  cryptoSubKey = DecStateR32(stream);
  cryptoAddr = DecStateR32(stream);
  cryptoReady = DecStateR8(stream);
  bb = (int)DecStateR32(stream);
  bb2 = (int)DecStateR32(stream);
  dec_hist = DecStateR16(stream);
  dec_header = DecStateR32(stream);
  buffer_pos = DecStateR32(stream);
  line_buffer_pos = DecStateR32(stream);
  line_buffer_size = DecStateR32(stream);
  done_compression = DecStateR8(stream);
  block_pos = DecStateR32(stream);
  block_size = DecStateR32(stream);
  block_numlines = DecStateR32(stream);
  buffer2a = DecStateR16(stream);
  MemStateRead(buffer2, 1, 2, stream);
  MemStateRead(buffer, 1, BUFFER_SIZE, stream);
  MemStateRead(line_buffer, 1, LINE_SIZE, stream);
  MemStateRead(line_buffer_prev, 1, LINE_SIZE, stream);

  /* A damaged state must not index outside the buffers. */
  bufferBit = (bb >= 0 && bb <= 7) ? bb : 7;
  bufferBit2 = (bb2 >= 0 && bb2 <= 15) ? bb2 : 15;
  if (buffer_pos > BUFFER_SIZE) buffer_pos = BUFFER_SIZE;
  if (line_buffer_size > LINE_SIZE) line_buffer_size = LINE_SIZE;
  if (line_buffer_pos > line_buffer_size) line_buffer_pos = line_buffer_size;
  return 0;
}

#define D5838_STATE_VERSION 1
#define D5838_STATE_SIZE (4 + 4 + 4 + 1 + 2 + 24*2 + 256 + 4*3 + 2 + 4)

/* 315-5838 family: uploaded tree/dictionary and the decoder position. */
void decathlt5838SaveState(void ** stream)
{
  int i;
  DecStateW32(D5838_STATE_VERSION, stream);
  DecStateW32(d5838.source, stream);
  DecStateW32((u32)d5838.abort_, stream);
  DecStateW8(d5838.bank, stream);
  DecStateW16(d5838.control, stream);
  for (i = 0; i < 24; i++)
    DecStateW16(d5838.tree[i], stream);
  MemStateWrite(d5838.dictionary, 1, 256, stream);
  DecStateW32((u32)d5838.tree_words, stream);
  DecStateW32((u32)d5838.dictionary_bytes, stream);
  DecStateW32((u32)d5838.upload_dictionary, stream);
  DecStateW16(d5838.word, stream);
  DecStateW32((u32)d5838.bits, stream);
}

int decathlt5838LoadState(const void * stream, int size)
{
  u32 version;
  int i, tw, db, bits;

  if (size < D5838_STATE_SIZE)
    return -1;
  version = DecStateR32(stream);
  if (version != D5838_STATE_VERSION)
    return -1;

  d5838.source = DecStateR32(stream) & 0x007FFFFF;
  d5838.abort_ = DecStateR32(stream) ? 1 : 0;
  d5838.bank = DecStateR8(stream) & 0x3;
  d5838.control = DecStateR16(stream);
  for (i = 0; i < 24; i++)
    d5838.tree[i] = DecStateR16(stream);
  MemStateRead(d5838.dictionary, 1, 256, stream);
  tw = (int)DecStateR32(stream);
  db = (int)DecStateR32(stream);
  d5838.upload_dictionary = DecStateR32(stream) ? 1 : 0;
  d5838.word = DecStateR16(stream);
  bits = (int)DecStateR32(stream);

  /* Cursors bounded like the upload code keeps them. */
  d5838.tree_words = (tw >= 0 && tw <= 24) ? tw : 0;
  d5838.dictionary_bytes = (db >= 0 && db <= 256) ? (db & ~1) : 0;
  d5838.bits = (bits >= 0 && bits <= 16) ? bits : 0;
  return 0;
}
