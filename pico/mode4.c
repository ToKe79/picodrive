/*
 * SMS renderer
 * (C) notaz, 2009-2010
 * (C) kub, 2021
 *
 * currently supports VDP mode 4 (SMS and GG) and mode 2
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */
/*
 * TODO:
 * - other TMS9918 modes?
 */
#include "pico_int.h"
#include <platform/common/upscale.h>

static void (*FinalizeLineSMS)(int line);
static int skip_next_line;
static int screen_offset, line_offset;
static u8 mode;

/* sprite collision detection */
static int CollisionDetect(u8 *mb, u16 sx, unsigned int pack, int zoomed)
{
  static u8 morton[16] = { 0x00,0x03,0x0c,0x0f,0x30,0x33,0x3c,0x3f,
                           0xc0,0xc3,0xcc,0xcf,0xf0,0xf3,0xfc,0xff };
  u8 *mp = mb + (sx>>3);
  unsigned col, m;

  // check sprite map for collision and update map with current sprite
  if (!zoomed) { // 8 sprite pixels
    m = mp[0] | (mp[1]<<8);
    col = m & (pack<<(sx&7)); // collision if current sprite overlaps sprite map
    m |= pack<<(sx&7);
    mp[0] = m, mp[1] = m>>8;
  } else { // 16 sprite pixels in zoom mode
    pack = morton[pack&0x0f] | (morton[(pack>>4)&0x0f] << 8);
    m = mp[0] | (mp[1]<<8) | (mp[2]<<16);
    col = m & (pack<<(sx&7));
    m |= pack<<(sx&7);
    mp[0] = m, mp[1] = m>>8, mp[2] = m>>16;
  }

  // invisible overscan area, not tested for collision
  mb[0] = mb[33] = 0;
  return col;
}

/* Mode 4 */
/*========*/

static void TileBGM4(u16 sx, int pal)
{
  u32 *pd = (u32 *)(Pico.est.HighCol + sx);
  pd[0] = pd[1] = pal * 0x01010101;
}

// 8 pixels are arranged in 4 bitplane bytes in a 32 bit word. To pull the
// 4 bitplanes together multiply with each bit distance (multiples of 1<<7)
#define PLANAR_PIXELBG(x,p) \
  t = (pack>>(7-p)) & 0x01010101; \
  t = (t*0x10204080) >> 28; \
  pd[x] = pal|t;

static void TileNormBGM4(u16 sx, unsigned int pack, int pal)
{
  u8 *pd = Pico.est.HighCol + sx;
  u32 t;

  PLANAR_PIXELBG(0, 0)
  PLANAR_PIXELBG(1, 1)
  PLANAR_PIXELBG(2, 2)
  PLANAR_PIXELBG(3, 3)
  PLANAR_PIXELBG(4, 4)
  PLANAR_PIXELBG(5, 5)
  PLANAR_PIXELBG(6, 6)
  PLANAR_PIXELBG(7, 7)
}

static void TileFlipBGM4(u16 sx, unsigned int pack, int pal)
{
  u8 *pd = Pico.est.HighCol + sx;
  u32 t;

  PLANAR_PIXELBG(0, 7)
  PLANAR_PIXELBG(1, 6)
  PLANAR_PIXELBG(2, 5)
  PLANAR_PIXELBG(3, 4)
  PLANAR_PIXELBG(4, 3)
  PLANAR_PIXELBG(5, 2)
  PLANAR_PIXELBG(6, 1)
  PLANAR_PIXELBG(7, 0)
}

// non-transparent sprite pixels apply if no higher prio pixel is already there
#define PLANAR_PIXELSP(x,p) \
  t = (pack>>(7-p)) & 0x01010101; \
  if (t && (pd[x] & 0x2f) <= 0x20) { \
    t = (t*0x10204080) >> 28; \
    pd[x] = pal|t; \
  }

static void TileNormSprM4(u16 sx, unsigned int pack, int pal)
{
  u8 *pd = Pico.est.HighCol + sx;
  u32 t;

  PLANAR_PIXELSP(0, 0)
  PLANAR_PIXELSP(1, 1)
  PLANAR_PIXELSP(2, 2)
  PLANAR_PIXELSP(3, 3)
  PLANAR_PIXELSP(4, 4)
  PLANAR_PIXELSP(5, 5)
  PLANAR_PIXELSP(6, 6)
  PLANAR_PIXELSP(7, 7)
}

static void TileDoubleSprM4(int sx, unsigned int pack, int pal)
{
  u8 *pd = Pico.est.HighCol + sx;
  u32 t;

  PLANAR_PIXELSP(0, 0)
  PLANAR_PIXELSP(1, 0)
  PLANAR_PIXELSP(2, 1)
  PLANAR_PIXELSP(3, 1)
  PLANAR_PIXELSP(4, 2)
  PLANAR_PIXELSP(5, 2)
  PLANAR_PIXELSP(6, 3)
  PLANAR_PIXELSP(7, 3)
  PLANAR_PIXELSP(8, 4)
  PLANAR_PIXELSP(9, 4)
  PLANAR_PIXELSP(10, 5)
  PLANAR_PIXELSP(11, 5)
  PLANAR_PIXELSP(12, 6)
  PLANAR_PIXELSP(13, 6)
  PLANAR_PIXELSP(14, 7)
  PLANAR_PIXELSP(15, 7)
}

static void DrawSpritesM4(int scanline)
{
  struct PicoVideo *pv = &Pico.video;
  unsigned char mb[1+256/8+2] = {0}; // zoomed
  unsigned int sprites_addr[8];
  unsigned int sprites_x[8];
  unsigned int pack;
  u8 *sat;
  int xoff = 8; // relative to HighCol, which is (screen - 8)
  int sprite_base, addr_mask;
  int zoomed = pv->reg[1] & 0x1; // zoomed sprites, e.g. Earthworm Jim
  int i, s, h, m;

  if (pv->reg[0] & 8)
    xoff = 0;
  xoff += line_offset;
  if ((Pico.m.hardware & 0x3) == 0x3)
    xoff -= 48; // GG LCD, adjust to center 160 px

  sat = (u8 *)PicoMem.vram + ((pv->reg[5] & 0x7e) << 7);
  if (pv->reg[1] & 2) {
    addr_mask = 0xfe; h = 16;
  } else {
    addr_mask = 0xff; h = 8;
  }
  if (zoomed) h *= 2;
  sprite_base = (pv->reg[6] & 4) << (13-2-1);

  for (i = s = 0; i < 64; i++)
  {
    int y;
    y = (sat[MEM_LE2(i)] + 1) & 0xff;
    if (y == 0xd1 && !((pv->reg[0] & 6) == 6 && (pv->reg[1] & 0x18)))
      break;
    if (y + h <= scanline || scanline < y)
      continue; // not on this line
    if (s >= 8) {
      pv->status |= SR_SOVR;
      break;
    }

    if (xoff + sat[MEM_LE2(0x80 + i*2)] >= 0) {
      sprites_x[s] = xoff + sat[MEM_LE2(0x80 + i*2)];
      sprites_addr[s] = sprite_base + ((sat[MEM_LE2(0x80 + i*2 + 1)] & addr_mask) << (5-1)) +
        ((scanline - y) >> zoomed << (2-1));
      s++;
    }
  }

  // now draw all sprites backwards
  m = 0;
  for (--s; s >= 0; s--) {
    pack = CPU_LE2(*(u32 *)(PicoMem.vram + sprites_addr[s]));
    if (zoomed) TileDoubleSprM4(sprites_x[s], pack, 0x10);
    else        TileNormSprM4(sprites_x[s], pack, 0x10);
    // make sprite pixel map by merging the 4 bitplanes
    pack = ((pack | (pack>>16)) | ((pack | (pack>>16))>>8)) & 0xff;
    if (!m)     m = CollisionDetect(mb, sprites_x[s], pack, zoomed);
  }
  if (m)
    pv->status |= SR_C;
}

// cells_dx, tilex_ty merged to reduce register pressure
static void DrawStripM4(const u16 *nametab, int cells_dx, int tilex_ty)
{
  int oldcode = -1;
  int addr = 0, pal = 0;

  // Draw tiles across screen:
  for (; cells_dx > 0; cells_dx += 8, tilex_ty++, cells_dx -= 0x10000)
  {
    unsigned int pack;
    unsigned code;

    code = nametab[tilex_ty & 0x1f];

    if (code != oldcode) {
      oldcode = code;
      // Get tile address/2:
      addr = (code & 0x1ff) << 4;
      addr += tilex_ty >> 16;
      if (code & 0x0400)
        addr ^= 0xe; // Y-flip

      pal = (code>>7) & 0x30;  // prio | palette select
    }

    pack = CPU_LE2(*(u32 *)(PicoMem.vram + addr)); // Get 4 bitplanes / 8 pixels
    if (pack == 0)          TileBGM4(cells_dx, pal);
    else if (code & 0x0200) TileFlipBGM4(cells_dx, pack, pal);
    else                    TileNormBGM4(cells_dx, pack, pal);
  }
}

static void DrawDisplayM4(int scanline)
{
  struct PicoVideo *pv = &Pico.video;
  u16 *nametab, *nametab2;
  int line, tilex, dx, ty, cells;
  int cellskip = 0; // XXX
  int maxcells = 32;

  // Find the line in the name table
  line = pv->reg[9] + scanline; // vscroll + scanline

  // Find name table:
  nametab = PicoMem.vram;
  if ((pv->reg[0] & 6) == 6 && (pv->reg[1] & 0x18)) {
    // 224/240 line mode
    line &= 0xff;
    nametab += ((pv->reg[2] & 0x0c) << (10-1)) + (0x700 >> 1);
  } else {
    while (line >= 224) line -= 224;
    nametab += (pv->reg[2] & 0x0e) << (10-1);
    // old SMS only, masks line:7 with reg[2]:0 for address calculation
    //if ((pv->reg[2] & 0x01) == 0) line &= 0x7f;
  }
  nametab2 = nametab + ((scanline>>3) << (6-1));
  nametab  = nametab + ((line>>3)     << (6-1));

  dx = pv->reg[8]; // hscroll
  if (scanline < 16 && (pv->reg[0] & 0x40))
    dx = 0; // hscroll disabled for top 2 rows (e.g. Fantasy Zone II)

  tilex = ((-dx >> 3) + cellskip) & 0x1f;
  ty = (line & 7) << 1; // Y-Offset into tile
  cells = maxcells - cellskip;

  dx = ((dx - 1) & 7) + 1;
  if (dx != 8)
    cells++; // have hscroll, need to draw 1 cell more
  dx += cellskip << 3;
  dx += line_offset;

  // tiles
  if (!(pv->debug_p & PVD_KILL_B)) {
    if ((Pico.m.hardware & 0x3) == 0x3) {
      // on GG render only the center 160 px
      DrawStripM4(nametab , dx | ((cells-12)<< 16),(tilex+6) | (ty  << 16));
    } else if (pv->reg[0] & 0x80) {
      // vscroll disabled for rightmost 8 columns (e.g. Gauntlet)
      int dx2 = dx + (cells-8)*8, tilex2 = tilex + (cells-8), ty2 = scanline&7;
      DrawStripM4(nametab,  dx | ((cells-8) << 16), tilex  | (ty  << 16));
      DrawStripM4(nametab2, dx2 |       (8  << 16), tilex2 | (ty2 << 17));
    } else
      DrawStripM4(nametab , dx | ( cells    << 16), tilex  | (ty  << 16));
  }

  // sprites
  if (!(pv->debug_p & PVD_KILL_S_LO))
    DrawSpritesM4(scanline);

  if ((pv->reg[0] & 0x20) && (Pico.m.hardware & 0x3) != 0x3) {
    // first column masked with background, caculate offset to start of line
    dx = (dx&~0x1f) / 4;
    ty = 0xe0e0e0e0; // really (pv->reg[7]&0x3f) * 0x01010101, but the looks...
    ((u32 *)Pico.est.HighCol)[dx+2] = ((u32 *)Pico.est.HighCol)[dx+3] = ty;
  }
}


/* TMS Modes */
/*===========*/

/* Background, Graphics modes */

#define TMS_PIXELBG(x,p) \
  t = (pack>>(7-p)) & 0x01; \
  t = (pal >> (t << 2)) & 0x0f; \
  pd[x] = t;

static void TileNormBgGr(u16 sx, unsigned int pack, int pal)
{
  u8 *pd = Pico.est.HighCol + sx;
  unsigned int t;

  TMS_PIXELBG(0, 0)
  TMS_PIXELBG(1, 1)
  TMS_PIXELBG(2, 2)
  TMS_PIXELBG(3, 3)
  TMS_PIXELBG(4, 4)
  TMS_PIXELBG(5, 5)
  TMS_PIXELBG(6, 6)
  TMS_PIXELBG(7, 7)
}

/* Sprites */

#define TMS_PIXELSP(x,p) \
  t = (pack>>(7-p)) & 0x01; \
  if (t) \
    pd[x] = pal;

static void TileNormSprTMS(u16 sx, unsigned int pack, int pal)
{
  u8 *pd = Pico.est.HighCol + sx;
  unsigned int t;

  TMS_PIXELSP(0, 0)
  TMS_PIXELSP(1, 1)
  TMS_PIXELSP(2, 2)
  TMS_PIXELSP(3, 3)
  TMS_PIXELSP(4, 4)
  TMS_PIXELSP(5, 5)
  TMS_PIXELSP(6, 6)
  TMS_PIXELSP(7, 7)
}

static void TileDoubleSprTMS(u16 sx, unsigned int pack, int pal)
{
  u8 *pd = Pico.est.HighCol + sx;
  unsigned int t;

  TMS_PIXELSP(0, 0)
  TMS_PIXELSP(1, 0)
  TMS_PIXELSP(2, 1)
  TMS_PIXELSP(3, 1)
  TMS_PIXELSP(4, 2)
  TMS_PIXELSP(5, 2)
  TMS_PIXELSP(6, 3)
  TMS_PIXELSP(7, 3)
  TMS_PIXELSP(8, 4)
  TMS_PIXELSP(9, 4)
  TMS_PIXELSP(10, 5)
  TMS_PIXELSP(11, 5)
  TMS_PIXELSP(12, 6)
  TMS_PIXELSP(13, 6)
  TMS_PIXELSP(14, 7)
  TMS_PIXELSP(15, 7)
}

/* Draw sprites into a scanline, max 4 */
static void DrawSpritesTMS(int scanline)
{
  struct PicoVideo *pv = &Pico.video;
  unsigned char mb[1+256/8+4] = {0}; // zoomed+doublesize
  unsigned int sprites_addr[4];
  unsigned int sprites_x[4];
  unsigned int pack;
  u8 *sat;
  int xoff = 8; // relative to HighCol, which is (screen - 8)
  int sprite_base, addr_mask;
  int zoomed = pv->reg[1] & 0x1; // zoomed sprites
  int i, s, h, m;

  xoff += line_offset;

  sat = (u8 *)PicoMem.vramb + ((pv->reg[5] & 0x7e) << 7);
  if (pv->reg[1] & 2) {
    addr_mask = 0xfc; h = 16;
  } else {
    addr_mask = 0xff; h = 8;
  }
  if (zoomed) h *= 2;

  sprite_base = (pv->reg[6] & 0x7) << 11;

  /* find sprites on this scanline */
  for (i = s = 0; i < 32; i++)
  {
    int y;
    y = (sat[MEM_LE2(4*i)] + 1) & 0xff;
    if (y == 0xd1)
      break;
    if (y > 0xe0)
      y -= 256;
    if (y + h <= scanline || scanline < y)
      continue; // not on this line
    if (s >= 4) {
      pv->status |= SR_SOVR | i;
      break;
    }

    sprites_x[s] = 4*i;
    sprites_addr[s] = sprite_base + ((sat[MEM_LE2(4*i + 2)] & addr_mask) << 3) +
      ((scanline - y) >> zoomed);
    s++;
  }

  // now draw all sprites backwards
  m = 0;
  for (--s; s >= 0; s--) {
    int x, c, w = (zoomed ? 16: 8);
    i = sprites_x[s];
    x = sat[MEM_LE2(i+1)] + xoff;
    if (sat[MEM_LE2(i+3)] & 0x80)
      x -= 32;
    c = sat[MEM_LE2(i+3)] & 0x0f;
    if (x > 0) {
      pack = PicoMem.vramb[MEM_LE2(sprites_addr[s])];
      if (zoomed) TileDoubleSprTMS(x, pack, c);
      else        TileNormSprTMS(x, pack, c);
      if (!m)     m = CollisionDetect(mb, x, pack, zoomed);
    }
    if((pv->reg[1] & 0x2) && (x+=w) > 0) {
      pack = PicoMem.vramb[MEM_LE2(sprites_addr[s]+0x10)];
      if (zoomed) TileDoubleSprTMS(x, pack, c);
      else        TileNormSprTMS(x, pack, c);
      if (!m)     m = CollisionDetect(mb, x, pack, zoomed);
    }
  }
  if (m)
    pv->status |= SR_C;
}

/* Mode 2 */
/*========*/

/* Draw the background into a scanline; cells, dx, tilex, ty merged to reduce registers */
static void DrawStripM2(const u8 *nametab, const u8 *coltab, const u8 *pattab, int cells_dx, int tilex_ty)
{
  // Draw tiles across screen:
  for (; cells_dx > 0; cells_dx += 8, tilex_ty++, cells_dx -= 0x10000)
  {
    unsigned int pack, pal;
    unsigned code;

    code = nametab[tilex_ty & 0x1f] << 3;
    pal  = coltab[code];
    pack = pattab[code];
    TileNormBgGr(cells_dx, pack, pal);
  }
}

/* Draw a scanline */
static void DrawDisplayM2(int scanline)
{
  struct PicoVideo *pv = &Pico.video;
  u8 *nametab, *coltab, *pattab;
  int tilex, dx, cells;
  int cellskip = 0; // XXX
  int maxcells = 32;

  // name, color, pattern table:
  nametab = PicoMem.vramb + ((pv->reg[2]<<10) & 0x3c00);
  coltab  = PicoMem.vramb + ((pv->reg[3]<< 6) & 0x2000);
  pattab  = PicoMem.vramb + ((pv->reg[4]<<11) & 0x2000);

  nametab += ((scanline>>3) << 5);
  coltab  += ((scanline>>6) <<11) + (scanline & 0x7);
  pattab  += ((scanline>>6) <<11) + (scanline & 0x7);

  tilex = cellskip & 0x1f;
  cells = maxcells - cellskip;
  dx = (cellskip << 3) + line_offset + 8;

  // tiles
  if (!(pv->debug_p & PVD_KILL_B))
    DrawStripM2(nametab, coltab, pattab, dx | (cells << 16), tilex | (scanline << 16));

  // sprites
  if (!(pv->debug_p & PVD_KILL_S_LO))
    DrawSpritesTMS(scanline);
}

/* Mode 1 */
/*========*/

/* Draw the background into a scanline; cells, dx, tilex, ty merged to reduce registers */
static void DrawStripM1(const u8 *nametab, const u8 *coltab, const u8 *pattab, int cells_dx, int tilex_ty)
{
  // Draw tiles across screen:
  for (; cells_dx > 0; cells_dx += 8, tilex_ty++, cells_dx -= 0x10000)
  {
    unsigned int pack, pal;
    unsigned code;

    code = nametab[tilex_ty & 0x1f];
    pal  = coltab[code >> 3];
    pack = pattab[code << 3];
    TileNormBgGr(cells_dx, pack, pal);
  }
}

/* Draw a scanline */
static void DrawDisplayM1(int scanline)
{
  struct PicoVideo *pv = &Pico.video;
  u8 *nametab, *coltab, *pattab;
  int tilex, dx, cells;
  int cellskip = 0; // XXX
  int maxcells = 32;

  // name, color, pattern table:
  nametab = PicoMem.vramb + ((pv->reg[2]<<10) & 0x3c00);
  coltab  = PicoMem.vramb + ((pv->reg[3]<< 6) & 0x3fc0);
  pattab  = PicoMem.vramb + ((pv->reg[4]<<11) & 0x3800);

  nametab += (scanline>>3) << 5;
  pattab  += (scanline & 0x7);

  tilex = cellskip & 0x1f;
  cells = maxcells - cellskip;
  dx = (cellskip << 3) + line_offset + 8;

  // tiles
  if (!(pv->debug_p & PVD_KILL_B))
    DrawStripM1(nametab, coltab, pattab, dx | (cells << 16), tilex | (scanline << 16));

  // sprites
  if (!(pv->debug_p & PVD_KILL_S_LO))
    DrawSpritesTMS(scanline);
}


/* Common/global */
/*===============*/

static void FinalizeLineRGB555SMS(int line);
static void FinalizeLine8bitSMS(int line);

void PicoFrameStartSMS(void)
{
  int lines = 192, columns = 256, loffs, coffs;
  skip_next_line = 0;
  loffs = screen_offset = 24; // 192 lines is really 224 with top/bottom bars
  Pico.est.rendstatus = PDRAW_32_COLS;

  // if mode changes make palette dirty since some modes switch to a fixed one
  if (mode != ((Pico.video.reg[0]&0x06) | (Pico.video.reg[1]&0x18))) {
    mode = (Pico.video.reg[0]&0x06) | (Pico.video.reg[1]&0x18);
    Pico.m.dirtyPal = 1;
  }

  // Copy LCD enable flag for easier handling
  Pico.m.hardware &= ~0x2;
  if (PicoIn.opt & POPT_EN_GG_LCD)
    Pico.m.hardware |= 0x2;

  if ((Pico.m.hardware & 0x3) == 0x3) {
    // GG LCD always has 160x144 regardless of settings
    screen_offset = 24; // nonetheless the vdp timing has 224 lines
    loffs = 48;
    lines = 144;
    columns = 160;
  } else switch (mode) {
  // SMS2 only 224/240 line modes, e.g. Micro Machines
  case 0x06|0x08:
      loffs = screen_offset = 0;
      lines = 240;
      break;
  case 0x06|0x10:
      loffs = screen_offset = 8;
      lines = 224;
      break;
  }
  if (PicoIn.opt & POPT_EN_SOFTSCALE) {
    coffs = 0;
    columns = 320;
  } else
    coffs = PicoIn.opt & POPT_DIS_32C_BORDER ? 0:(320-columns)/2;
  line_offset = (FinalizeLineSMS == NULL ? coffs : 0);

  if (FinalizeLineSMS == FinalizeLineRGB555SMS)
    line_offset = 0 /* done in FinalizeLine */;

  if (Pico.est.rendstatus != rendstatus_old || lines != rendlines) {
    emu_video_mode_change(loffs, lines, coffs, columns);
    rendstatus_old = Pico.est.rendstatus;
    rendlines = lines;
  }

  Pico.est.HighCol = HighColBase + screen_offset * HighColIncrement;
  Pico.est.DrawLineDest = (char *)DrawLineDestBase + screen_offset * DrawLineDestIncrement;

  if (FinalizeLineSMS == FinalizeLine8bitSMS) {
    Pico.est.SonicPalCount = 0;
    Pico.m.dirtyPal = (Pico.m.dirtyPal ? 2 : 0);
    memcpy(Pico.est.SonicPal, PicoMem.cram, 0x20*2);
  }
}

void PicoLineSMS(int line)
{
  int skip = skip_next_line;

  // GG LCD, render only visible part of screen
  if ((Pico.m.hardware & 0x3) == 0x3 && (line < 24 || line >= 24+144))
    goto norender;

  if (PicoScanBegin != NULL && skip == 0)
    skip = PicoScanBegin(line + screen_offset);

  if (skip) {
    skip_next_line = skip - 1;
    return;
  }

  // Draw screen:
  BackFill(Pico.video.reg[7] & 0x0f, 0, &Pico.est);
  if (Pico.video.reg[1] & 0x40) {
    if      (Pico.video.reg[0] & 0x04) DrawDisplayM4(line);
    else if (Pico.video.reg[0] & 0x02) DrawDisplayM2(line);
    else                               DrawDisplayM1(line);
  }

  if (FinalizeLineSMS != NULL)
    FinalizeLineSMS(line);

  if (PicoScanEnd != NULL)
    skip_next_line = PicoScanEnd(line + screen_offset);

norender:
  Pico.est.HighCol += HighColIncrement;
  Pico.est.DrawLineDest = (char *)Pico.est.DrawLineDest + DrawLineDestIncrement;
}

/* Fixed palette for TMS9918 modes */
static u16 tmspal[32] = {
  0x0000, 0x0000, 0x00a0, 0x00f0, 0x0500, 0x0f00, 0x0005, 0x0ff0,
  0x000a, 0x000f, 0x0055, 0x00ff, 0x0050, 0x0f0f, 0x0555, 0x0fff,
};

void PicoDoHighPal555SMS(void)
{
  u32 *spal = (void *)Pico.est.SonicPal;
  u32 *dpal = (void *)Pico.est.HighPal;
  unsigned int cnt = Pico.est.SonicPalCount+1;
  unsigned int t;
  int i, j;
 
  if (FinalizeLineSMS != FinalizeLine8bitSMS || Pico.m.dirtyPal == 2)
    Pico.m.dirtyPal = 0;

  // use hardware palette for 16bit accurate mode
  if (FinalizeLineSMS == FinalizeLineRGB555SMS)
    spal = (void *)PicoMem.cram;

  // fixed palette in TMS modes
  if (!(Pico.video.reg[0] & 0x4)) {
    spal = (u32 *)tmspal;
    cnt = 1;
  }

  /* SMS 6 bit cram data was already converted to MD/GG format by vdp write,
   * hence GG/SMS/TMS can all be handled the same here */
  for (j = cnt; j > 0; j--) {
    for (i = 0x20/2; i > 0; i--, spal++, dpal++) { 
     t = *spal;
#if defined(USE_BGR555)
     t = ((t & 0x000f000f)<< 1) | ((t & 0x00f000f0)<<2) | ((t & 0x0f000f00)<<3);
     t |= (t >> 4) & 0x04210421;
#elif defined(USE_BGR565)
     t = ((t & 0x000f000f)<< 1) | ((t & 0x00f000f0)<<3) | ((t & 0x0f000f00)<<4);
     t |= (t >> 4) & 0x08610861;
#else
     t = ((t & 0x000f000f)<<12) | ((t & 0x00f000f0)<<3) | ((t & 0x0f000f00)>>7);
     t |= (t >> 4) & 0x08610861;
#endif
     *dpal = t;
    }
    memcpy(dpal, dpal-0x20/2, 0x20*2); // for prio bit
    spal += 0x20/2, dpal += 0x20/2;
  }
  Pico.est.HighPal[0xe0] = 0;
}

static void FinalizeLineRGB555SMS(int line)
{
  if (Pico.m.dirtyPal)
    PicoDoHighPal555SMS();

  // standard FinalizeLine can finish it for us,
  // with features like scaling and such
  FinalizeLine555(0, line, &Pico.est);
}

static void FinalizeLine8bitSMS(int line)
{
  FinalizeLine8bit(0, line, &Pico.est);
}

void PicoDrawSetOutputSMS(pdso_t which)
{
  switch (which)
  {
    case PDF_8BIT:   FinalizeLineSMS = FinalizeLine8bitSMS; break;
    case PDF_RGB555: FinalizeLineSMS = FinalizeLineRGB555SMS; break;
    // there's no fast renderer yet, just treat it like PDF_8BIT
    default:         FinalizeLineSMS = FinalizeLine8bitSMS;
                     PicoDrawSetInternalBuf(Pico.est.Draw2FB, 328); break;
  }
  rendstatus_old = -1;
  mode = -1;
}

// vim:shiftwidth=2:ts=2:expandtab
