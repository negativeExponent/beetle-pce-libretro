/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*  VCE and VPC emulation */

/*
<_Ki>	So here is what I found today: the /Hblank signal is not affected (at all) by VCE's dot clock selection.
  It seems to be completely fixed;  /HBlank-L-period = 237 x master clock cycles, and /HBlank-H-period = 1128, and 
  that makes h-period = 1365 x master clock cycles as a whole.
*/

#include "pce.h"
#include "huc.h"
#include <mednafen/video.h>
#include "vce.h"
#include <mednafen/hw_video/huc6270/vdc.h>
#include "pcecd.h"
#include <math.h>
#include <mednafen/settings.h>


vce_resolution_t vce_resolution;

static bool hires;
static int scanline_start, scanline_end=242;

static struct
{
	int pulse;
	int start;
	int width;
	int end;

	int rate;
	int line;
} scanline_info[16];

static int scanline_info_count;


static const int vce_ratios[4] = { 4, 3, 2, 2 };

static int32 MDFN_FASTCALL NO_INLINE Sync(const int32 timestamp);

static void IRQChange_Hook(bool newstatus)
{
	extern VCE *vce; //HORRIBLE
	vce->IRQChangeCheck();
}

bool VCE::WS_Hook(int32 vdc_cycles)
{
	bool ret = true;
	int32 to_steal;

	if(vdc_cycles == -1) // Special event-based wait-stating
		to_steal = CalcNextEvent();
	else
		to_steal = ((vdc_cycles * dot_clock_ratio - clock_divider) + 2) / 3;

	if(to_steal <= 0) // This should never happen.  But in case it does...
		to_steal = 1;

	if((to_steal + ws_counter) > 455 * 64)
	{
		to_steal = 455 * 64 - ws_counter;

		if(to_steal < 0)
			to_steal = 0;

		ret = false;
	}

	if(to_steal > 0)
	{
		HuCPU.StealCycles(to_steal);
		ws_counter += to_steal;
	}

	return(ret);
}

static bool WS_Hook_(int32 vdc_cycles)
{
	extern VCE *vce;

	return(vce->WS_Hook(vdc_cycles));
}

void VCE::write_scanline_info()
{
	int HSR, HDR;
	int HSW, HDS, HDW, HDE;


	if(NeedSLReset || scanline_info_count == 0)
	{
		scanline_info_count = 0;

		memset(&vce_resolution, 0, sizeof(vce_resolution));
	}


	HSR = vdc[0].GetRegister(VDC::GSREG_HSR);
	HDR = vdc[0].GetRegister(VDC::GSREG_HDR);

	HSW = (HSR >> 0) & 0x1f;
	HDS = (HSR >> 8) & 0x7f;
	HDW = (HDR >> 0) & 0x7f;
	HDE = (HDR >> 8) & 0x7f;

	
	scanline_info[scanline_info_count].pulse = HSW;
	scanline_info[scanline_info_count].start = HDS;
	scanline_info[scanline_info_count].width = HDW;
	scanline_info[scanline_info_count].end = HDE;

	scanline_info[scanline_info_count].rate = dot_clock;
	scanline_info[scanline_info_count].line = scanline;
	scanline_info_count++;


	if((dot_clock > vce_resolution.max_rate) || (vce_resolution.width == 0))
	{
		switch(dot_clock)
		{
			case 0: vce_resolution.res_256 = 1; break;
			case 1: vce_resolution.res_352 = 1; break;
			case 2:
			case 3: vce_resolution.res_512 = 1; break;
		}

		vce_resolution.max_rate = dot_clock;

		vce_resolution.start = HDS;
		vce_resolution.width = HDW;
		
		vce_resolution.pulse = HSW;
		vce_resolution.end = HDE;
	}
}

void VCE::IRQChangeCheck(void)
{
	unsigned chip;
	bool irqtmp = 0;

	for(chip = 0; chip < chip_count; chip++)
	irqtmp |= vdc[chip].PeekIRQ();

	if(irqtmp)
		HuCPU.IRQBegin(HuC6280::IQIRQ1);
	else
		HuCPU.IRQEnd(HuC6280::IQIRQ1);
}

void VCE::SetShowHorizOS(bool show)
{
	ShowHorizOS = show;
}


VCE::VCE(const bool want_sgfx, const uint32 vram_size)
{
	unsigned chip;
	ShowHorizOS = false;

	sgfx        = want_sgfx;
	chip_count  = sgfx ? 2 : 1;

	cd_event    = 1;

	fb          = NULL;
	pitch32     = 0;

	for(chip = 0; chip < chip_count; chip++)
	{
		vdc[chip].SetVRAMSize(vram_size);
		vdc[chip].SetIRQHook(IRQChange_Hook);
		vdc[chip].SetWSHook(WS_Hook_);
		vdc[chip].SetLayerEnableMask(0x3);
	}

	SetVDCUnlimitedSprites(false);

	memset(surf_clut, 0, sizeof(surf_clut));

	SetShowHorizOS(false);
}

void VCE::SetVDCUnlimitedSprites(const bool nospritelimit)
{
	unsigned chip;
	for(chip = 0; chip < chip_count; chip++)
		vdc[chip].SetUnlimitedSprites(nospritelimit);
}

VCE::~VCE()
{

}

void VCE::Reset(const int32 timestamp)
{
	uint16_t i;
	unsigned chip;
	last_ts          = 0;

	pixel_offset     = 0;
	dot_clock        = 0;
	dot_clock_ratio  = vce_ratios[dot_clock];
	clock_divider    = 0;

	ws_counter       = 0;
	scanline         = 0;
	scanline_out_ptr = NULL;
	CR               = 0;
	lc263            = 0;
	bw               = 0;

	memset(color_table_cache, 0, sizeof(color_table_cache));

	for(i = 0; i < 0x200; i++)
	{
		color_table[i] = ((i ^ (i >> 3)) & 1) ? 0x000 : 0x1FF;
		FixPCache(i);
	}

	ctaddress        = 0;

	hblank           = 1;
	vblank           = 1;

	NeedSLReset      = false;

	hblank_counter   = 237;
	vblank_counter   = 4095 + 30;

	for(chip = 0; chip < chip_count; chip++)
		child_event[chip] = vdc[chip].Reset();

	// SuperGrafx VPC init
	priority[0] = 0x11;
	priority[1] = 0x11;
	winwidths[0] = 0;
	winwidths[1] = 0;
	st_mode = 0;
	window_counter[0] = 0x40;
	window_counter[1] = 0x40;

	if(fb)
		scanline_out_ptr = &fb[(scanline % 263) * pitch32];
}

/*
 Note:  If we're skipping the frame, don't write to the data behind the pXBuf, DisplayRect, and LineWidths
 pointers at all.  There's no need to, and HES playback depends on these structures being left alone; if they're not,
 there will be graphics distortion, and maybe memory corruption.
*/

void VCE::StartFrame(MDFN_Surface *surface, MDFN_Rect *DisplayRect, int32 *LineWidths, int skip)
{
	FrameDone      = false;

	color_table_cache[0x200] = color_table_cache[0x300] = MAKECOLOR(0x00, 0xFE, 0x00, 0);


	hires          = MDFN_GetSettingUI("pce.scaling") == 2;
	scanline_start = MDFN_GetSettingUI("pce.slstart");
	scanline_end   = MDFN_GetSettingUI("pce.slend");


	if(!skip)
	{
		uint16_t y;
		DisplayRect->x   = 0;
		DisplayRect->w   = 1365;
		DisplayRect->y   = 0 + 14;
		DisplayRect->h   = max_T<uint32>(240, scanline_end + 1); //263 - 14;

		DisplayRect->y   = 14 + scanline_start;
		DisplayRect->h   = scanline_end - scanline_start + 1;

		for(y = 0; y < 263; y++)
			LineWidths[y] = 0;

		pitch32          = surface->pitch;
		fb               = surface->pixels;
		LW               = LineWidths;
		scanline_out_ptr = &fb[(scanline % 263) * pitch32];
	}
	else
	{
		pitch32          = 0;
		fb               = NULL;
		LW               = NULL;
		scanline_out_ptr = NULL;
	}

	skipframe = skip;
}

bool VCE::RunPartial(void)
{
	HuCPU.SetEventHandler(Sync);

	if(!PCE_IsCD)
		cd_event = 0x3FFFFFFF;

	ws_counter = 0;
	HuCPU.Run();

	if(!skipframe)
	{
		uint16_t y;
		// Worst-case fallback
		int32 LW_Fix = 256;

		for(y = 0; y < 263; y++)
		{
			if(LW[y])
			{
				LW_Fix = LW[y];
				break;
			}
		}

		for(y = 0; y < 263; y++)
		{
			if(!LW[y])
				LW[y] = LW_Fix;
		}
	}

	Update(HuCPU.Timestamp());

	return(FrameDone);
}

void VCE::Update(const int32 timestamp)
{
	if(PCE_IsCD)
		SetCDEvent(PCECD_Run(timestamp));

	HuCPU.SetEvent(Sync(timestamp));
}

int32 INLINE VCE::CalcNextEvent(void)
{
	int32 next_event = hblank_counter;

	if(next_event > vblank_counter)
		next_event = vblank_counter;

	if(next_event > cd_event)
		next_event = cd_event;

	next_event = min_T<int32>(next_event, child_event[0] * dot_clock_ratio - clock_divider);

	if(sgfx)
		next_event = min_T<int32>(next_event, child_event[1] * dot_clock_ratio - clock_divider);

	if(next_event < 1)
		next_event = 1;

	return next_event;
}

template<bool TA_SuperGrafx, bool TA_AwesomeMode>
void INLINE VCE::SyncSub(int32 clocks)
{
	while(clocks > 0)
	{
		int32 div_clocks;
		int32 chunk_clocks = clocks;

		if(chunk_clocks > hblank_counter)
			chunk_clocks = hblank_counter;

		if(chunk_clocks > vblank_counter)
			chunk_clocks = vblank_counter;

		chunk_clocks = min_T<int32>(chunk_clocks, child_event[0] * dot_clock_ratio - clock_divider);

		if(TA_SuperGrafx)
			chunk_clocks = min_T<int32>(chunk_clocks, child_event[1] * dot_clock_ratio - clock_divider);

		if(MDFN_UNLIKELY(chunk_clocks <= 0))
			chunk_clocks = 1;
 
		clock_divider  += chunk_clocks;
		div_clocks      = clock_divider / dot_clock_ratio;
		clock_divider  -= div_clocks * dot_clock_ratio;

		child_event[0] -= div_clocks;
		if(TA_SuperGrafx)
			child_event[1] -= div_clocks;

		if(div_clocks > 0)
		{
			bool skipline = false;

			if((scanline < 14 + scanline_start) || (scanline > 14 + scanline_end))
				skipline = true;

			child_event[0] = vdc[0].Run(div_clocks, pixel_buffer[0], skipline);
			if(TA_SuperGrafx)
				child_event[1] = vdc[1].Run(div_clocks, pixel_buffer[1], skipline);

			if(!skipline)
			{
				if(TA_SuperGrafx)
				{
					int32_t i;
					for(i = 0; MDFN_LIKELY(i < div_clocks); i++)
					{
						static const int prio_select[4] = { 1, 1, 0, 0 };
						static const int prio_shift[4] = { 4, 0, 4, 0 };
						uint32 pix;
						int in_window = 0;

						if(window_counter[0] > 0x40)
						{
							in_window |= 1;
							window_counter[0]--;
						}

						if(window_counter[1] > 0x40)
						{
							in_window |= 2;
							window_counter[1]--;
						}

						uint8 pb = (priority[prio_select[in_window]] >> prio_shift[in_window]) & 0xF;
						uint32 vdc2_pixel, vdc1_pixel;

						vdc2_pixel = vdc1_pixel = 0;

						if(pb & 1)
							vdc1_pixel = pixel_buffer[0][i];
						if(pb & 2)
							vdc2_pixel = pixel_buffer[1][i];

						/* Dai MakaiMura uses setting 1, and expects VDC #2 sprites in front of VDC #1 background, but
							behind VDC #1's sprites.
						*/
						switch(pb >> 2)
						{
						case 1:
							if((vdc2_pixel & 0x100) && !(vdc1_pixel & 0x100) && (vdc2_pixel & 0xF))
								vdc1_pixel = 0; //amask;
							break;
						case 2:
							if((vdc1_pixel & 0x100) && !(vdc2_pixel & 0x100) && (vdc2_pixel & 0xF))
								vdc1_pixel = 0; //|= amask;
							break;
						}
						pix = color_table_cache[((vdc1_pixel & 0xF) ? vdc1_pixel : vdc2_pixel) & 0x1FF];

						if(TA_AwesomeMode)
						{
							int32_t s_i;
							for(s_i = 0; s_i < dot_clock_ratio; s_i++)
							{
								scanline_out_ptr[pixel_offset & 2047] = pix;
								pixel_offset++;
							}
						}
						else
						{
							scanline_out_ptr[pixel_offset & 2047] = pix;
							pixel_offset++;
						}
					}
				}
				else
				{
					if(TA_AwesomeMode)
					{
						int32_t i;
						for(i = 0; MDFN_LIKELY(i < div_clocks); i++)
						{
							int32_t si;
							for(si = 0; si < dot_clock_ratio; si++)
							{
								uint32 pix = color_table_cache[pixel_buffer[0][i] & 0x3FF];
						
								scanline_out_ptr[pixel_offset & 2047] = pix;
								pixel_offset++;
							}
						}
					}
					else
					{
						int32_t i;
						for(i = 0; MDFN_LIKELY(i < div_clocks); i++)
						{
							uint32 pix = color_table_cache[pixel_buffer[0][i] & 0x3FF];
							scanline_out_ptr[pixel_offset & 2047] = pix;
							pixel_offset++;
						}
					}
				}
			}	// end if(!skipframe)
		} // end if(div_clocks > 0)

		clocks -= chunk_clocks;
		hblank_counter -= chunk_clocks;
		if(hblank_counter <= 0)
		{
			hblank ^= 1;
  
			if(hblank)
			{
				// Clock gets stretched and "synchronized" at the beginning of the 237-master-cycle hsync period.
				//clock_divider = 0;
			}
			else
			{
				if(sgfx)
				{
					int add = 8 + ((dot_clock == 1) ? 38 : 24);
					window_counter[0] = winwidths[0] + add;
					window_counter[1] = winwidths[1] + add;
				}

				if(NeedSLReset)
				{
					scanline = 0;
					
					write_scanline_info();
				}
				else
					scanline++;

				if(scanline == 14 + max_T<uint32>(240, scanline_end + 1))
					FrameDone = true;

				if((scanline == 14 + max_T<uint32>(240, scanline_end + 1)) || (scanline == 123))
					HuCPU.Exit();

				scanline_out_ptr = &fb[(scanline % 263) * pitch32];

				pixel_offset = 0;
				NeedSLReset = false;

				if(!skipframe)
				{
					static const int x_offsets[2][4] = {
						{ 8 + 24,      8 + 38,      8 + 96,      8 + 96 },
						{ 8 + 24 - 12, 8 + 38 - 16, 8 + 96 - 24, 8 + 96 - 24 },
					};
					static const int w_cows[2][4] = {
						{ 256,      341,      512,      512 },
						{ 256 + 24, 341 + 32, 512 + 48, 512 + 48 },
					};

					int rect_x, rect_w;

					if(TA_AwesomeMode)
					{
						if(dot_clock >= 2)
							rect_x = 208;
						else if(dot_clock == 1)
							rect_x = 136;
						else
							rect_x = 128;
						rect_w = 1024;

						if(ShowHorizOS)
						{
							rect_x -= 48;
							rect_w += 96;
						}
					}
					else
					{
						rect_x = x_offsets[ShowHorizOS][dot_clock]; 
						rect_w = w_cows[ShowHorizOS][dot_clock];
					}

					pixel_offset = (0 - rect_x) & 2047;
					LW[scanline % 263] = rect_w;
				}
			}
			hblank_counter = hblank ? 237 : 1128;

			child_event[0] = vdc[0].HSync(hblank);
			if(TA_SuperGrafx)
				child_event[1] = vdc[1].HSync(hblank);
		}

		vblank_counter -= chunk_clocks;
		if(vblank_counter <= 0)
		{
			vblank ^= 1;
			vblank_counter = vblank ? 4095 : ((lc263 ? 358995 : 357630) - 4095);

			if(!vblank)
			{
				NeedSLReset = true;
			}

			child_event[0] = vdc[0].VSync(vblank);
			if(TA_SuperGrafx)
				child_event[1] = vdc[1].VSync(vblank);
		}
	}
}

// If we ignore the return value of Sync(), we must do "HuCPU.SetEvent(CalcNextEvent());"
// before the function(read/write functions) that called Sync() return!
int32 INLINE VCE::SyncReal(const int32 timestamp)
{
	int32 clocks = timestamp - last_ts;

	cd_event -= clocks;
	if(cd_event <= 0)
		cd_event = PCECD_Run(timestamp);

	if(!hires)
	{
		if(sgfx)
			SyncSub<true, false>(clocks);
		else
			SyncSub<false, false>(clocks);
	}
	else
	{
		if(sgfx)
			SyncSub<true, true>(clocks);
		else
			SyncSub<false, true>(clocks);
	}

	//
	//
	//
	int32 ret = CalcNextEvent();

	last_ts = timestamp;

	return(ret);
}

static int32 MDFN_FASTCALL NO_INLINE Sync(const int32 timestamp)
{
	extern VCE *vce; //HORRIBLE
	return vce->SyncReal(timestamp);
}

// So wrong, but feels so...MUSHROOMY.
// THIS IS BROKEN!
// We need to put Sync() call before that, or bias
// cd_event and the value to HuCPU.SetEvent by (HuCPU.timestamp - last_ts)
void VCE::SetCDEvent(const int32 cycles)
{
	const int32 time_behind = HuCPU.Timestamp() - last_ts;

	assert(time_behind >= 0);

	cd_event = cycles + time_behind;
	HuCPU.SetEvent(CalcNextEvent() - time_behind);
}

void VCE::FixPCache(int entry)
{
	const uint32* csl = surf_clut[bw];

	if(!(entry & 0xFF))
	{
		uint8_t x;
		for(x = 0; x < 16; x++)
			color_table_cache[(entry & 0x100) + (x << 4)] = csl[color_table[entry & 0x100]];
	}

	if(!(entry & 0xF))
		return;

	color_table_cache[entry] = csl[color_table[entry]];
}

void VCE::SetVCECR(uint8 V)
{
	if(((V & 0x80) >> 7) != bw)
	{
		uint16_t x;
		bw = V & 0x80;
		for(x = 0; x < 512; x++)
			FixPCache(x);
	}

	lc263 = (V & 0x04);

	dot_clock = V & 0x3;
	dot_clock_ratio = vce_ratios[dot_clock];
	
	CR = V;

	write_scanline_info();
}

void VCE::SetPixelFormat(const uint8* CustomColorMap, const uint32 CustomColorMapLen)
{
	uint16_t x;
	for(x = 0; x < 512; x++)
	{
		int r, g, b;
		int sc_r, sc_g, sc_b;

		if(CustomColorMap)
		{
			r = CustomColorMap[x * 3 + 0];
			g = CustomColorMap[x * 3 + 1];
			b = CustomColorMap[x * 3 + 2];
		}
		else
		{
			b = 36 * (x & 0x007);
			r = 36 * ((x & 0x038) >> 3);
			g = 36 * ((x & 0x1c0) >> 6);
		}

		if(CustomColorMap && CustomColorMapLen == 1024)
		{
			sc_r = CustomColorMap[(512 + x) * 3 + 0];
			sc_g = CustomColorMap[(512 + x) * 3 + 1];
			sc_b = CustomColorMap[(512 + x) * 3 + 2];
		}
		else
		{
			double y = floor(0.5 + 0.300 * r + 0.589 * g + 0.111 * b);
			if(y < 0)
				y = 0;

			if(y > 255)
				y = 255;

			sc_r = sc_g = sc_b = y;
		}

		surf_clut[0][x] = MAKECOLOR(r, g, b, 0);
		surf_clut[1][x] = MAKECOLOR(sc_r, sc_g, sc_b, 0);
	}

	// I know the temptation is there, but 
        // don't combine these two loops just
	// because they loop 512 times ;)
	for(x = 0; x < 512; x++)
		FixPCache(x);
}

uint8 VCE::Read(uint32 A)
{
	uint8 ret = 0xFF;

	Sync(HuCPU.Timestamp());

	switch(A & 0x7)
	{
	case 4:
		ret = color_table[ctaddress & 0x1FF];
		break;

	case 5:
		ret = color_table[ctaddress & 0x1FF] >> 8;
		ret &= 1;
		ret |= 0xFE;
		ctaddress = (ctaddress + 1) & 0x1FF;
		break;
	}

	HuCPU.SetEvent(CalcNextEvent());

	return(ret);
}

void VCE::Write(uint32 A, uint8 V)
{
	Sync(HuCPU.Timestamp());

	switch(A&0x7)
	{
	case 0:
	{
		int old_dot_clock = dot_clock;

		SetVCECR(V);

		if(old_dot_clock != dot_clock)	// FIXME, this is wrong.  A total fix will require changing the meaning
										// of clock_divider variable.
		{
			clock_divider = 0;
		}
	}
	break;

	case 2: ctaddress &= 0x100; ctaddress |= V; break;
	case 3: ctaddress &= 0x0FF; ctaddress |= (V & 1) << 8; break;

	case 4:
		color_table[ctaddress & 0x1FF] &= 0x100;
		color_table[ctaddress & 0x1FF] |= V;
		FixPCache(ctaddress & 0x1FF);
		break;

	case 5:
		color_table[ctaddress & 0x1FF] &= 0xFF;
		color_table[ctaddress & 0x1FF] |= (V & 1) << 8;
		FixPCache(ctaddress & 0x1FF);
		ctaddress = (ctaddress + 1) & 0x1FF;
		break;
	}

	HuCPU.SetEvent(CalcNextEvent());
}

uint8 VCE::ReadVDC(uint32 A)
{
	uint8 ret;

	Sync(HuCPU.Timestamp());

	if(!sgfx)
	{
		ret = vdc[0].Read(A, child_event[0], 0);
	}
	else
	{
		int chip = 0;

		A &= 0x1F;

		if(A & 0x8)
		{
			ret = 0;

			switch(A)
			{
			case 0x8: ret = priority[0]; break;
			case 0x9: ret = priority[1]; break;
			case 0xA: ret = winwidths[0]; break;
			case 0xB: ret = winwidths[0] >> 8; break;
			case 0xC: ret = winwidths[1]; break;
			case 0xD: ret = winwidths[1] >> 8; break;
			case 0xE: ret = 0; break;
			}
		}
		else
		{
			chip = (A & 0x10) >> 4;
			ret = vdc[chip].Read(A & 0x3, child_event[chip], 0);
		}
	}

	HuCPU.SetEvent(CalcNextEvent());

	return(ret);
}

void VCE::WriteVDC(uint32 A, uint8 V)
{
	Sync(HuCPU.Timestamp());

	if(!sgfx)
	{
		vdc[0].Write(A & 0x1FFF, V, child_event[0]);
	}
	else
	{
		int chip = 0;

		// For ST0/ST1/ST2
		A |= ((A >> 31) & st_mode) << 4;

		A &= 0x1F;

		if(A & 0x8)
		{
			switch(A)
			{
			case 0x8: priority[0] = V; break;
			case 0x9: priority[1] = V; break;
			case 0xA: winwidths[0] &= 0x300; winwidths[0] |= V; break;
			case 0xB: winwidths[0] &= 0x0FF; winwidths[0] |= (V & 3) << 8; break;
			case 0xC: winwidths[1] &= 0x300; winwidths[1] |= V; break;
			case 0xD: winwidths[1] &= 0x0FF; winwidths[1] |= (V & 3) << 8; break;
			case 0xE: st_mode = V & 1; break;
			}
		}
		else
		{
			chip = (A & 0x10) >> 4;
			vdc[chip].Write(A & 0x3, V, child_event[chip]);
		}
	}

	HuCPU.SetEvent(CalcNextEvent());
}

void VCE::WriteVDC_ST(uint32 A, uint8 V)
{
	Sync(HuCPU.Timestamp());

	if(!sgfx)
	{
		vdc[0].Write(A, V, child_event[0]);
	}
	else
	{
		int chip = st_mode & 1;
		vdc[chip].Write(A, V, child_event[chip]);
	}

	HuCPU.SetEvent(CalcNextEvent());
}

void VCE::SetLayerEnableMask(uint64 mask)
{
	for(unsigned chip = 0; chip < chip_count; chip++)
	{
		vdc[chip].SetLayerEnableMask((mask >> (chip * 2)) & 0x3);
	}
}

int VCE::StateAction(StateMem *sm, const unsigned load, const bool data_only)
{
	SFORMAT VCE_StateRegs[] =
	{
		SFVARN(CR, "VCECR"),

		SFVAR(ws_counter),

		SFVARN(ctaddress, "ctaddress"),
		SFVARN(color_table, "color_table"),

		SFVARN(clock_divider, "clock_divider"),
		SFVARN(child_event, "child_event"),
		SFVARN(scanline, "scanline"),
		SFVARN(pixel_offset, "pixel_offset"),
		SFVARN(hblank_counter, "hblank_counter"),
		SFVARN(vblank_counter, "vblank_counter"),
		SFVAR(hblank),
		SFVAR(vblank),
		SFVAR(NeedSLReset),
		SFEND
	};

	int ret = MDFNSS_StateAction(sm, load, data_only, VCE_StateRegs, "VCE", false);

	if(sgfx)
	{
		SFORMAT VPC_StateRegs[] =
		{
			SFVARN(priority, "priority"),
			SFVARN(winwidths, "winwidths"),
			SFVARN(st_mode, "st_mode"),
			SFVARN(window_counter, "window_counter"),
			SFEND
		};

		ret &= MDFNSS_StateAction(sm, load, data_only, VPC_StateRegs, "VPC", false);
	}

	if(load)
	{
		SetVCECR(CR);
		//
		//
		//
		ctaddress &= 0x1FF;

		clock_divider = (uint32)clock_divider % dot_clock_ratio;

		if(scanline < 0)
			scanline = 0;

		if(hblank_counter < 1)
			hblank_counter = 1;
		else if(hblank_counter > 1365)
			hblank_counter = 1365;

		if(vblank_counter < 1)
			vblank_counter = 1;
		else if(vblank_counter > 400000)
			vblank_counter = 400000;

		if(cd_event < 1)
			cd_event = 1;

		for(unsigned chip = 0; chip < chip_count; chip++)
		{
			if(child_event[chip] < 1)
				child_event[chip] = 1;
			else if(child_event[chip] > 1024)
				child_event[chip] = 1024;
		}
		//
		//
		//
		for(int x = 0; x < 512; x++)
			FixPCache(x);
	}

	for(unsigned chip = 0; chip < chip_count; chip++)
		ret &= vdc[chip].StateAction(sm, load, data_only, chip ? "VDCB" : "VDC");
	
	return ret;
}

void VCE::EndFrame(MDFN_Rect *DisplayRect)
{
	bool scale_hires = hires;
	bool scale_lores = MDFN_GetSettingUI("pce.scaling") == 1;

	write_scanline_info();

	if(!scale_hires && (vce_resolution.res_256 + vce_resolution.res_352 + vce_resolution.res_512) > 1)
	{
		static const int horiz_draw[2][4] = {
			{ 256, 341, 512, 512 },
			{ 256 + 24, 341 + 32, 512 + 48, 512 + 48 }
		};
		int lcv;

		for(lcv = 0; lcv < scanline_info_count - 1; lcv++)
		{
			int scale, width;
			int rate = scanline_info[lcv].rate;

			if(!vce_resolution.res_352)
			{
				// 512 multi-res
				if(rate == 0)
					scale = 2;
				else
					continue;
			}
			else if(scale_lores)
			{
				if(rate == 0)
					scale = 1;
				else
					continue;
			}
			else
			{
				// 1024 multi-res
				scale = vce_ratios[rate];
				scale_hires = true;
			}

			width = horiz_draw[ShowHorizOS][rate];


			int start_y = scanline_info[lcv].line;
			int end_y = scanline_info[lcv + 1].line;

			for(int line = end_y - start_y - 1; line >= 0; line--)
			{
				bpp_t* line_ptr = &fb[(start_y + line) * pitch32];

				switch(scale)
				{
					case 4:
						for(int pixel = width - 1; pixel >= 0; pixel--)
						{
							line_ptr[pixel * 4 + 0] = line_ptr[pixel];
							line_ptr[pixel * 4 + 1] = line_ptr[pixel];
							line_ptr[pixel * 4 + 2] = line_ptr[pixel];
							line_ptr[pixel * 4 + 3] = line_ptr[pixel];
						}
						break;

					case 3:
						for(int pixel = width - 1; pixel >= 0; pixel--)
						{
							line_ptr[pixel * 3 + 0] = line_ptr[pixel];
							line_ptr[pixel * 3 + 1] = line_ptr[pixel];
							line_ptr[pixel * 3 + 2] = line_ptr[pixel];
						}
						break;

					case 2:
						for(int pixel = width - 1; pixel >= 0; pixel--)
						{
							line_ptr[pixel * 2 + 0] = line_ptr[pixel];
							line_ptr[pixel * 2 + 1] = line_ptr[pixel];
						}
						break;

					case 1:
					{
						int adjust = (horiz_draw[ShowHorizOS][1] - horiz_draw[ShowHorizOS][0]) / 2;

						memmove(line_ptr + adjust, line_ptr, width * sizeof(*fb));
						memset(line_ptr, 0, adjust * sizeof(*fb));
						memset(line_ptr + width + adjust, 0, (adjust + 1) * sizeof(*fb));
						break;
					}
				}
			}
		} // scanline_info_count
	} // !hires && multi_res


	int rate = vce_resolution.max_rate;
	int width = (vce_resolution.width + 1) * 8;
	int start = vce_resolution.start * 8;

	vce_resolution.width = width;

	if(MDFN_GetSettingB("pce.crop_h_overscan"))
	{
		static const int horiz_start[] = { 16, 40, 80 };
		static const int horiz_width[] = { 256, 320, 512 };
		static const int horiz_over[]  = { 280, 376, 608 };

		// Mednafen overscan: 256 - 341.3 - 512 // 280 - 373.3 - 560 = clean 1024 dot clock scaling math
		static const int horiz_adjust[] = { 32 - 24, (59 - 32) + 2, (160 - 48) + 16, (160 - 48) + 16 };

		/*
		Horizontal Overscan
 		2  2 31 3 `` 16 256 16 = 288 `` 12 256 12 = 280    Bonk's Adventure
 		2  2 31 4 `` 16 256 16 = 288 `` 12 256 12 = 280    240p test suite (*)
		2  3 31 4 `` 24 256  8 = 288 `` 20 256  4 = 280    Super Darius
        -----------------------------------------------    Final Blaster


		3  6 37 7 `` 48 304 48 = 400 `` 34 304 35 = 373    Wizardry
		3  5 39 6 `` 40 320 40 = 400 `` 26 320 27 = 373    240p test suite (*)
		3  4 41 6 `` 32 336 32 = 400 `` 18 336 19 = 373    R-Type I (U)
		3  3 43 6 `` 24 352 24 = 400 `` 10 352 11 = 373    Ninja Spirit
		2  3 43 3 `` 24 352 24 = 400 `` 10 352 11 = 373    TV Sports Basketball  (16px left/right crop)
		9  4 43 4 `` 32 352 16 = 400 `` xx 352 xx = 373    Addams Family         (376 + overdraw)
		2  5 43 6 `` 40 352  8 = 400 `` xx 352 xx = 373    Asuka 120% dialogue   (376 + overdraw)
		2  4 45 6 `` 32 368  0 = 400 ``  2 368  3 = 373    Asuka 120% logo       (376 + overdraw)


		5 10 63 8 `` 80 512 80 = 672 `` 24 512 24 = 560    240p test suite (*)
		2 12 63 0 `` 96 512 64 = 672 `` 40 512  8 = 560    Asuka 120% multi-res
		0  8 67 0 `` 64 544 64 = 672 ``  8 544  8 = 560    TV Sports Basketball
		*/

		if(width >= horiz_width[rate])
		{
			if(width + start > horiz_over[rate])
				width = horiz_over[rate] - start;

			DisplayRect->x = start;
			DisplayRect->w = width;
		}
		else
		{
			DisplayRect->x = horiz_start[rate];
			DisplayRect->w = horiz_width[rate];
		}

		DisplayRect->x -= horiz_adjust[rate] / 2;


		if(scale_hires)
		{
			DisplayRect->x *= vce_ratios[rate];
			DisplayRect->w *= vce_ratios[rate];

			if(rate == 1)
				DisplayRect->x += 2;

			vce_resolution.max_rate = 4;
		}
	}
	else
	{
		DisplayRect->x = 0;

		if(!scale_hires)
		{
			switch(rate)
			{
				case 0: DisplayRect->w = 256 + (ShowHorizOS ? 24 : 0); break;
				case 1: DisplayRect->w = 341 + (ShowHorizOS ? 32 : 0) + 2; break;
				case 2:
				case 3: DisplayRect->w = 512 + (ShowHorizOS ? 48 : 0); break;
			}
		}
		else
		{
			DisplayRect->w = 1024 + (ShowHorizOS ? 96 : 0);

			vce_resolution.max_rate = 4;
		}
	}

	DisplayRect->y = 14 + scanline_start;
	DisplayRect->h = scanline_end - scanline_start + 1;
}
