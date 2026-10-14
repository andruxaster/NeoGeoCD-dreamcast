/***************************************************************************
 *   NeoCD - The Neo Geo CD emulator                                       *
 *   Copyright (C) 2008 by Fabrice Martinez                                *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef _YM2610_H_
#define _YM2610_H_

#include <stdint.h>
#include <stdbool.h>

/*
 for busy flag emulation , function FM_GET_TIME_NOW() should be
 return the present time in master clock cycles
 in timer.c
 */
uint64_t ngcd_ym_time_now(void);   /* master clock cycles */
/* provided by the host: bring the output up to the current time before a
   register write changes the sound */
void YM2610UpdateRequest(void);
#define FM_GET_TIME_NOW() ngcd_ym_time_now()

typedef int16_t FMSAMPLE;
typedef int32_t FMSAMPLE_MIX;

/* The chip hands its timer loads over as counts of its own clock
   ticks; what a tick is in host time is the caller's business.
*/
typedef void(*FM_TIMERHANDLER) (int channel, int count);
typedef void(*FM_IRQHANDLER) (int irq);

int YM2610Init(int baseclock, int rate,
	void *pcmroma, int pcmsizea,
#if YM2610_ADPCMB
	void *pcmromb, int pcmsizeb,
#endif
	FM_TIMERHANDLER TimerHandler,
	FM_IRQHANDLER IRQHandler);

void  YM2610Reset(void);
/* render length stereo samples (L,R interleaved) */
void  YM2610UpdateTo(int16_t *out, int length);
int   YM2610Write(int addr, uint8_t value);
uint8_t YM2610Read(int addr);
int   YM2610TimerOver(int channel);


#endif /* _YM2610_H_ */
