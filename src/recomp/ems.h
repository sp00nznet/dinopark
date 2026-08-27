/*
 * ems.h - expanded memory (LIM EMS), which DinoPark pages its assets through.
 *
 * The game finds the driver by opening the character device "EMMXXXX0" and
 * checking it with two IOCTLs, then asks INT 67h where the page frame is. Both
 * halves are needed: without the device the INT 67h calls never happen.
 *
 * DINO_EMS=0 turns it off; DINO_EMS_KB sets how much to offer.
 */
#ifndef DINO_EMS_H
#define DINO_EMS_H

#include "cpu.h"

int      ems_enabled(void);
uint16_t ems_frame_segment(void);
void     ems_int67(CPU *cpu);

#endif
