/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Yeo Kheng Meng */
#ifndef UVC_SIM_GENERATOR_H
#define UVC_SIM_GENERATOR_H
#define UVCSIM_WIDTH 320u
#define UVCSIM_HEIGHT 240u
#define UVCSIM_BYTES (UVCSIM_WIDTH * UVCSIM_HEIGHT * 2u)
/* Original fixed-size YUY2 synthetic image with an explicit SIMULATED label.
 * Caller owns storage. Returns bytes written, or0 without writing on error. */
unsigned int UvcSimGenerate(unsigned char *bytes, unsigned int capacity,
                           unsigned int frame);
#endif
