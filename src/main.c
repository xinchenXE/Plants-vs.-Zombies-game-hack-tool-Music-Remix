/* Funky Stars RMX (ROR keygen version) - 32-bit PE32 player
 *
 * 程序里内嵌整份 FastTracker 2 (.xm) 模块，运行期自己解析并回放：
 *   10 声道、8-bit 采样、线性插值、Amiga 周期表、音量包络 + fadeout。
 * 只实现这个模块实际用到的效果（8 panning / 4 vibrato /
 * A 音量滑 / 3 tone porta，以及音量列的 设定音量 / 音量滑 / 声像），
 * 其余一律忽略 —— 省体积。
 *
 * 没有 CRT、没有 libc、没有音频文件。
 */
#include "mod_data.h"
#include "tables.h"

typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned short u16;
typedef short          s16;
typedef unsigned int   u32;
typedef int            s32;
typedef unsigned long long u64;

typedef void *HWO;
typedef char *LPSTR;

typedef struct { u16 tag, ch; u32 rate, bps; u16 align, bits, cb; } WFX;
typedef struct { LPSTR data; u32 len, rec, user, flags, loops; void *next; u32 res; } HDR;

int  __stdcall waveOutOpen(HWO *h, u32 id, const WFX *f, u32 cb, u32 inst, u32 flags);
int  __stdcall waveOutPrepareHeader(HWO h, HDR *p, u32 n);
int  __stdcall waveOutUnprepareHeader(HWO h, HDR *p, u32 n);
int  __stdcall waveOutWrite(HWO h, HDR *p, u32 n);
int  __stdcall waveOutReset(HWO h);
int  __stdcall waveOutClose(HWO h);
u32  __stdcall GetTickCount(void);
void __stdcall Sleep(u32 ms);
int  _kbhit(void);
int  _getch(void);
void * __stdcall GetStdHandle(s32 n);
int  __stdcall GetConsoleMode(void *h, u32 *m);
int  __stdcall SetConsoleMode(void *h, u32 m);
int  __stdcall WriteFile(void *h, const void *buf, u32 n, u32 *done, void *ov);
void __stdcall ExitProcess(u32 code);

#ifndef GAIN
#define GAIN 189
#endif

#define NCH 10
#define MAXS 24

/* ---------------- 模块解析结果 ---------------- */
static u8  g_ord[256];
static u32 g_patOff[40];
static s8  g_smpBuf[29000];
static const s8 *g_smp[MAXS];
static const u8 *g_insHdr[MAXS];       /* 指向乐器头，读包络参数用 */
static s32 g_smpLen[MAXS], g_smpLoop[MAXS], g_smpLL[MAXS];
static s32 g_smpVol[MAXS], g_smpPan[MAXS];
static s32 g_nCh = NCH, g_nPat = 30, g_nIns = 22, g_songLen = 33, g_restart = 3;
static s32 g_speed = 6, g_bpm = 130;
static s32 g_tickRate;                 /* 每 tick 的采样数，16.16 */

/* ---------------- 通道状态 ---------------- */
typedef struct {
	s32 pos, step, len, end, loop, loopLen;   /* 16.16 */
	const s8 *p;
	s32 vol, pan, tL, tR, aL, aR;
	s32 per, tper, porta;
	s32 vibP, vibS, vibD, vibOn;
	s32 slide;
	s32 env, epos, fade, keyoff;
	s32 inst, note, arp;
	s32 on;
} Ch;

static Ch g_ch[NCH];

#ifdef AUDIT
static u32 g_acnt[MAXS];
static s32 g_apeak[MAXS];
static u32 g_anote[MAXS];
#endif

/* ---------------- 播放位置 ---------------- */
static const u8 *g_pp;
static s32 g_row, g_tick, g_ordIdx, g_ended;
static u32 g_tickAcc;

/* FT2 正弦表（32 点，幅度 127） */
static const s8 g_vib[32] = {
	0, 25, 49, 71, 90, 106, 118, 126, 127, 126, 118, 106,
	90, 71, 49, 25, 0, -25, -49, -71, -90, -106, -118, -126,
	-127, -126, -118, -106, -90, -71, -49, -25
};

/* ---------------- 工具 ---------------- */
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u32 rd16(const u8 *p) { return p[0] | (p[1] << 8); }

static void zero(void *p, u32 n)
{
	u8 *d = (u8 *)p;
	while (n--) *d++ = 0;
}

/* 16.16 步进：step = g_instK[inst] / period（纯 32 位除法） */
static s32 calcStep(s32 inst, s32 per)
{
	if (per < 16) per = 16;
	return (s32)(g_instK[inst] / (u32)per);
}

/* ---------------- 解析模块 ---------------- */
static s32 initMod(void)
{
	const u8 *d = g_mod;
	s32 o = 17 + 20 + 1 + 20, i, c;
	u32 hs;
	s32 total = 0;
	const u8 *sp;

	o += 2;                        /* version */
	hs = rd32(d + o); o += 4;
	g_songLen = rd16(d + o); o += 2;
	g_restart = rd16(d + o); o += 2;
	g_nCh     = rd16(d + o); o += 2;
	g_nPat    = rd16(d + o); o += 2;
	g_nIns    = rd16(d + o); o += 2;
	o += 2;                        /* flags */
	g_speed = rd16(d + o); o += 2;
	g_bpm   = rd16(d + o); o += 2;
	for (i = 0; i < 256; i++) g_ord[i] = d[o + i];
	o += 256;
	if (hs > 276) o += hs - 276;

	for (i = 0; i < g_nPat; i++) {
		u32 ph = rd32(d + o);
		u32 sz = rd16(d + o + 7);
		g_patOff[i] = o + ph;
		o += ph + sz;
	}

	/* 采样：每个乐器一个采样；先把 delta 编码的数据解开 */
	for (i = 0; i < g_nIns && i < MAXS; i++) {
		u32 isz = rd32(d + o);
		u32 ns = rd16(d + o + 27);
		g_insHdr[i] = d + o;
		g_smpLen[i] = g_smpLoop[i] = g_smpLL[i] = 0;
		g_smpVol[i] = 64;
		g_smpPan[i] = 128;
		if (ns) {
			u32 shs = rd32(d + o + 29);
			s32 p = o + isz;
			s32 len = rd32(d + p);
			g_smpLen[i]  = len;
			g_smpLoop[i] = rd32(d + p + 4);
			g_smpLL[i]   = rd32(d + p + 8);
			g_smpVol[i]  = d[p + 12];
			g_smpPan[i]  = d[p + 15];
			/* type bit0 才是"循环"开关。不循环的采样 loop/looplen 里可能残留
			   非零值，照 looplen 判循环会让这些音永不停止 —— 持续嗡鸣。 */
			if (!(d[p + 14] & 1)) g_smpLL[i] = 0;
			p += shs * ns;
			g_smp[i] = g_smpBuf + total;   /* 指向解码缓冲区（稍后填） */
			sp = d + p;
			{
				s8 old = 0;
				for (c = 0; c < len; c++) {
					old = (s8)(old + (s8)sp[c]);
					g_smpBuf[total + c] = old;
				}
			}
			total += len;
			p += len * ns;
			o = p;
		} else {
			g_smp[i] = g_smpBuf;
			o += isz;
		}
	}

	for (i = 0; i < g_nCh && i < NCH; i++) {
		Ch *k = &g_ch[i];
		zero(k, sizeof(Ch));
		k->pan = 128;
		k->env = 64;
		k->fade = 65536;
	}

	/* tick 长度 = 2.5/bpm 秒 */
	g_tickRate = (s32)(((u64)110250 << 16) / g_bpm);

	g_ordIdx = 0;
	g_row = 0;
	g_tick = 0;
	g_tickAcc = 0;
	g_pp = g_mod + g_patOff[g_ord[0]];
	return 1;
}

/* ---------------- 音量包络 ---------------- */
static s32 envVal(const u8 *p, s32 n, s32 x)
{
	s32 i, x0, y0, x1, y1;
	x0 = rd16(p); y0 = rd16(p + 2);
	if (x <= x0) return y0;
	for (i = 1; i < n; i++) {
		x1 = rd16(p + i * 4);
		if (x <= x1) {
			y1 = rd16(p + i * 4 + 2);
			if (x1 == x0) return y1;
			return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
		}
		x0 = x1; y0 = rd16(p + i * 4 + 2);
	}
	return y0;
}

static void updEnv(Ch *k)
{
	const u8 *h = g_insHdr[k->inst];
	s32 vt = h[233], n = h[225];
	if (!(vt & 1) || n < 2) { k->env = 64; return; }
	k->epos++;
	if (!k->keyoff && (vt & 2)) {
		s32 sx = rd16(h + 129 + h[227] * 4);
		if (k->epos > sx) k->epos = sx;
	} else if (vt & 4) {
		if (k->epos > (s32)rd16(h + 129 + h[229] * 4))
			k->epos = rd16(h + 129 + h[228] * 4);
	}
	k->env = envVal(h + 129, n, k->epos);
}

static void updAmp(Ch *k)
{
	s32 v = k->vol * k->env / 64;
	v = v * g_smpVol[k->inst] / 64;
	v = (v * k->fade) >> 16;
	v = (v * GAIN) >> 8;                    /* 整体增益标定（对齐参考 RMS） */
	k->tL = v * (255 - k->pan);             /* 8.8 定点：ramp 用小数累积，避免小音量被吃掉 */
	k->tR = v * k->pan;
}

/* ---------------- 行/效果处理 ---------------- */
static void startNote(Ch *k, s32 n, s32 ins, s32 vol)
{
	s32 i = ins;
	if (i >= g_nIns) i = g_nIns - 1;
	k->inst = i;
	if (!g_smpLen[i]) return;               /* 空乐器：不动当前音 */
	k->p = g_smp[i];
	k->len = g_smpLen[i] << 16;
	k->loop = g_smpLoop[i] << 16;
	k->loopLen = g_smpLL[i] << 16;
	k->end = (g_smpLL[i] ? g_smpLoop[i] + g_smpLL[i] : g_smpLen[i]) << 16;
	k->pos = 0;
	k->vol = vol ? vol - 0x10 : 64;
	k->pan = g_smpPan[i];
	k->per = g_period[n - 1];
	k->note = n;
	k->arp = 0;
	k->tper = 0;
	k->porta = 0;
	k->vibP = 0;
	k->slide = 0;
	k->vibOn = 0;
	k->epos = -1;                           /* 下一个 tick 落到 0 */
	k->fade = 65536;
	k->keyoff = 0;
	k->step = calcStep(i, k->per);
	k->on = 1;
#ifdef AUDIT
	g_anote[i]++;
#endif
}

#ifdef TRACE
static void putb(const void *p, u32 n) { u32 d; WriteFile(GetStdHandle(-11), p, n, &d, 0); }
#endif

static void doRow(void)
{
	s32 c;
#ifdef TRACE
	{
		u8 mk[3]; mk[0] = 0xFF; mk[1] = (u8)g_ordIdx; mk[2] = (u8)g_row;
		putb(mk, 3);
	}
#endif
	for (c = 0; c < g_nCh; c++) {
		Ch *k = &g_ch[c];
		u8 b = *g_pp++, n = 0, ins = 0, v = 0, e = 0, pa = 0;

		k->slide = 0;
		k->vibOn = 0;
		k->arp = 0;
	
		if (b & 0x80) {
			if (b & 1)  n = *g_pp++;
			if (b & 2)  ins = *g_pp++;
			if (b & 4)  v = *g_pp++;
			if (b & 8)  e = *g_pp++;
			if (b & 16) pa = *g_pp++;
		} else {
			n = b; ins = *g_pp++; v = *g_pp++; e = *g_pp++; pa = *g_pp++;
		}

#ifdef TRACE
		{
			u8 cell[5]; cell[0] = n; cell[1] = ins; cell[2] = v; cell[3] = e; cell[4] = pa;
			putb(cell, 5);
		}
#endif

		if (n == 97) {                          /* key off */
			n = 0;
			if (k->on) {
				if (g_insHdr[k->inst][233] & 1) k->keyoff = 1;
				else { k->on = 0; k->tL = k->tR = 0; }
			}
		}

		if (n) {
			/* tone porta 只在"通道还活着且没被 key off"时才是滑音。
			   已经 OFF 的通道再来 note+toneporta，等于重新起音 ——
			   否则音量会卡在 key off 后的低包络上，听感就是整段掉音。 */
			if (e == 3 && k->on && !k->keyoff) {
				k->tper = g_period[n - 1];
			} else {
				startNote(k, n, ins ? ins - 1 : k->inst, v);
			}
		} else if (ins) {
			k->inst = ins - 1;
		}

		/* --- 效果列 --- */
		if (e == 8) {
			k->pan = pa;
		} else if (e == 4) {
			if (pa) { k->vibS = pa >> 4; k->vibD = pa & 15; }
			k->vibOn = 1;
		} else if (e == 0xA) {
			k->slide = (pa >> 4) - (pa & 15);
		} else if (e == 3) {
			/* 3xy：x 是上行速度、y 是下行速度，单位 4 个周期/tick。
			   方向由 per 和 tper 的大小关系决定，这里只存幅度。 */
			k->porta = ((pa >> 4) ? (pa >> 4) : (pa & 15)) * 4;
			if (!k->tper && n) k->tper = g_period[n - 1];
		} else if (e == 0) {
			/* 0xy = arpeggio：每个 tick 在 原音 / +x / +y 之间轮转。
			   这个模块用了 352 次（037 小三和弦、047 大三和弦各 176 次），
			   整条和声层都靠它 —— 漏掉就只剩单音。 */
			k->arp = pa;
		}

		/* --- 音量列 --- */
		if (v >= 0x10 && v <= 0x50) {
			k->vol = (v - 0x10);
		} else if (v >= 0x60 && v <= 0x6F) {
			k->slide = -(v & 15);
		} else if (v >= 0x70 && v <= 0x7F) {
			k->slide = (v & 15);
		} else if (v >= 0xC0 && v <= 0xCF) {
			k->pan = (v & 15) * 17;
		}
		if (k->vol < 0) k->vol = 0;
		if (k->vol > 64) k->vol = 64;
	}
}

static void doTick(void)
{
	s32 c;
	for (c = 0; c < g_nCh; c++) {
		Ch *k = &g_ch[c];
		s32 per;
		if (!k->on) continue;

		if (k->slide) {
			k->vol += k->slide;
			if (k->vol < 0) k->vol = 0;
			if (k->vol > 64) k->vol = 64;
		}
		if (k->tper) {
			if (k->per < k->tper) {
				k->per += k->porta;
				if (k->per > k->tper) k->per = k->tper;
			} else if (k->per > k->tper) {
				k->per -= k->porta;
				if (k->per < k->tper) k->per = k->tper;
			}
		}

		per = k->per;
		if (k->arp) {
			/* tick%3：0 = 原音，1 = +x，2 = +y（FT2 的轮转顺序） */
			s32 t = g_tick % 3, nn = k->note;
			if (t == 1) nn += k->arp >> 4;
			else if (t == 2) nn += k->arp & 15;
			if (nn < 1) nn = 1;
			if (nn > 96) nn = 96;
			per = g_period[nn - 1];
		}
		if (k->vibOn && k->vibD) {
			k->vibP += k->vibS;
			per += (g_vib[k->vibP & 31] * k->vibD) >> 7;
		}
		k->step = calcStep(k->inst, per);
	}
}

/* 每 tick：推进包络 + 更新音量 */
static void envAll(void)
{
	s32 c;
	for (c = 0; c < g_nCh; c++) {
		Ch *k = &g_ch[c];
		if (!k->on) continue;
		updEnv(k);
		if (k->keyoff) {
			k->fade -= rd16(g_insHdr[k->inst] + 239);
			if (k->fade <= 0) { k->fade = 0; k->on = 0; k->tL = k->tR = 0; continue; }
		}
		updAmp(k);
	}
}

static void nextOrder(void)
{
	if (g_ordIdx + 1 >= g_songLen) g_ended = 1;
	g_ordIdx++;
	if (g_ordIdx >= g_songLen) g_ordIdx = g_restart;
	g_pp = g_mod + g_patOff[g_ord[g_ordIdx]];
	g_row = 0;
}

#ifdef STATE
static void dumpState(void)
{
	s32 c;
	u8 buf[3 + 5 * NCH];
	buf[0] = (u8)g_ordIdx; buf[1] = (u8)g_row; buf[2] = (u8)g_tick;
	for (c = 0; c < NCH; c++) {
		u32 st = (u32)(g_ch[c].on ? g_ch[c].step : 0);
		buf[3 + c * 5 + 0] = (u8)(st);
		buf[3 + c * 5 + 1] = (u8)(st >> 8);
		buf[3 + c * 5 + 2] = (u8)(st >> 16);
		buf[3 + c * 5 + 3] = (u8)(st >> 24);
		buf[3 + c * 5 + 4] = (u8)g_ch[c].vol;
	}
	{ u32 d; WriteFile(GetStdHandle(-11), buf, sizeof(buf), &d, 0); }
}
#endif

static void tick(void)
{
	if (g_tick == 0) doRow();
	else doTick();
	envAll();
#ifdef STATE
	if (g_tick == 0) dumpState();
#endif
	if (++g_tick >= g_speed) {
		g_tick = 0;
		if (++g_row >= 64) nextOrder();
	}
}

/* ---------------- 混音 ---------------- */
#ifdef STEMS
static s16 g_stem[NCH];
#endif
static void mix(s16 *out, s32 frames)
{
	while (frames--) {
		s32 al = 0, ar = 0, c;
#ifdef STEMS
		for (c = 0; c < NCH; c++) g_stem[c] = 0;
#endif

		g_tickAcc += 65536;
		while (g_tickAcc >= (u32)g_tickRate) {
			g_tickAcc -= (u32)g_tickRate;
			tick();
		}

		for (c = 0; c < g_nCh; c++) {
			Ch *k = &g_ch[c];
			s32 s, idx, fr, s0, s1;

			if (!k->on) continue;

			idx = k->pos >> 16;
			fr = (k->pos >> 8) & 255;
			s0 = k->p[idx];
			/* 插值右邻点：走到循环末点后要接回循环起点，
			   否则每个循环周期都会出现一个保持住的样本 —— 短循环下就是细碎爆音。 */
			s1 = (idx + 1 < (k->end >> 16)) ? k->p[idx + 1]
			   : (k->loopLen ? k->p[k->loop >> 16] : s0);
			s = s0 + (((s1 - s0) * fr) >> 8);

			k->aL += (k->tL - k->aL) >> 4;
			k->aR += (k->tR - k->aR) >> 4;
			al += (s * k->aL) >> 8;
			ar += (s * k->aR) >> 8;
#ifdef STEMS
			g_stem[c] = (s16)((s * k->aL) >> 8);
#endif
#ifdef AUDIT
			{
				s32 a = (s * k->aL) >> 8;
				if (a < 0) a = -a;
				g_acnt[k->inst]++;
				if (a > g_apeak[k->inst]) g_apeak[k->inst] = a;
			}
#endif

			k->pos += k->step;
			if (k->pos >= k->end) {
				if (k->loopLen > 0) {
					while (k->pos >= k->end) k->pos -= k->loopLen;
				} else {
					k->on = 0;
				}
			}
		}

		if (al > 32767) al = 32767; else if (al < -32768) al = -32768;
		if (ar > 32767) ar = 32767; else if (ar < -32768) ar = -32768;
		out[0] = (s16)al;
		out[1] = (s16)ar;
		out += 2;
	}
}

/* ---------------- 输出 ---------------- */
/* 16 块 × 0.25 秒 = 4 秒的预读。每块播完立刻补下一块，
   始终维持满队列 —— 这样即使系统被抢占了零点几秒也不会断流。
   （之前是 0.5 秒一块、每 0.5 秒才补一块，只领先约 1 秒，
     一旦掉队驱动就会反复播同一小段，听起来就是"卡死的电流声"。） */
#define CHUNK 11025
#define QN    16
#define WHDR_DONE 1

static HWO g_hwo;
static HDR g_hdr[QN];
static s16 g_buf[QN][CHUNK * 2];

#if defined(STATE)
void _start(void)
{
	s32 i;
	if (!initMod()) ExitProcess(1);
	for (i = 0; i < 30000 && !g_ended; i++) {
		s32 j;
		for (j = 0; j < g_speed; j++) tick();
	}
	ExitProcess(0);
}
#elif defined(TRACE)
void _start(void)
{
	s32 i;
	if (!initMod()) ExitProcess(1);
	for (i = 0; i < 40000 && !g_ended; i++) {
		s32 j;
		for (j = 0; j < g_speed; j++) tick();
	}
	ExitProcess(0);
}
#elif !defined(RENDERTEST) && !defined(STEMS) && !defined(AUDIT)
void _start(void)
{
	WFX wf;
	s32 i;

	if (!initMod()) return;

	/* 关掉控制台"快速编辑"：否则鼠标一点窗口选中文字，整个进程会被挂起，
	   音频立刻断流 —— 就是那种"卡死的电流声"。 */
	{
		void *hc = GetStdHandle(-10);
		u32 m;
		if (GetConsoleMode(hc, &m)) SetConsoleMode(hc, (m & ~0x40u) | 0x80u);
	}

	wf.tag = 1; wf.ch = 2; wf.rate = 44100; wf.bps = 44100 * 4;
	wf.align = 4; wf.bits = 16; wf.cb = 0;
	if (waveOutOpen(&g_hwo, (u32)-1, &wf, 0, 0, 0)) return;
	for (i = 0; i < QN; i++) {
		g_hdr[i].data = (LPSTR)g_buf[i];
		g_hdr[i].len = CHUNK * 4;
		waveOutPrepareHeader(g_hwo, &g_hdr[i], sizeof(HDR));
	}

	tick();

	/* 预热：先把 4 秒全部塞进去 */
	for (i = 0; i < QN; i++) {
		mix(g_buf[i], CHUNK);
		waveOutWrite(g_hwo, &g_hdr[i], sizeof(HDR));
	}

	/* 轮询：任何一块播完就立刻补上，永远保持满队列。
	   写失败不退出（退出就等于停喂，驱动会反复播最后一块 = 电流声），
	   下一轮再试。 */
	for (;;) {
		s32 any = 0;
		if (_kbhit()) {
			int k = _getch();
			if (k == 'q' || k == 'Q') break;
		}
		for (i = 0; i < QN; i++) {
			HDR *h = &g_hdr[i];
			if (!(h->flags & WHDR_DONE)) continue;
			h->flags &= ~WHDR_DONE;
			mix(g_buf[i], CHUNK);
			if (!waveOutWrite(g_hwo, h, sizeof(HDR))) any = 1;
		}
		if (!any) Sleep(4);
	}

	waveOutReset(g_hwo);
	for (i = 0; i < QN; i++) waveOutUnprepareHeader(g_hwo, &g_hdr[i], sizeof(HDR));
	waveOutClose(g_hwo);
}
#elif defined(AUDIT)
void _start(void)
{
	void *h; u32 n; s32 i, f, frames = 44100 * 260; s16 dummy[2];
	u8 out[24 * 12];
	if (!initMod()) ExitProcess(1);
	h = GetStdHandle(-11);
	tick();
	for (f = 0; f < frames; f++) mix(dummy, 1);
	for (i = 0; i < 24; i++) {
		const u8 *p = (const u8 *)&g_anote[i];
		const u8 *q = (const u8 *)&g_acnt[i];
		const u8 *r = (const u8 *)&g_apeak[i];
		out[i * 12 + 0] = p[0]; out[i * 12 + 1] = p[1]; out[i * 12 + 2] = p[2]; out[i * 12 + 3] = p[3];
		out[i * 12 + 4] = q[0]; out[i * 12 + 5] = q[1]; out[i * 12 + 6] = q[2]; out[i * 12 + 7] = q[3];
		out[i * 12 + 8] = r[0]; out[i * 12 + 9] = r[1]; out[i * 12 + 10] = r[2]; out[i * 12 + 11] = r[3];
	}
	WriteFile(h, out, sizeof(out), &n, 0);
	ExitProcess(0);
}
#elif defined(STEMS)
void _start(void)
{
	void *h; u32 n; s32 i; s16 dummy[2];
	if (!initMod()) ExitProcess(1);
	h = GetStdHandle(-11);
	tick();
	for (i = 0; i < 44100 * 40; i++) {
		mix(dummy, 1);
		WriteFile(h, g_stem, NCH * 2, &n, 0);
	}
	ExitProcess(0);
}
#elif defined(RENDERTEST)
/* 渲染测试：把整曲写成裸 PCM 到 stdout，交给 Python 包 WAV 做对比 */
static s16 g_out[2048];
void _start(void)
{
	void *h;
	u32 n;
	s32 frames = 0;

	if (!initMod()) ExitProcess(1);
	h = GetStdHandle(-11);
	tick();
	while (!g_ended) {
		mix(g_out, 1024);
		frames = 1024;
		WriteFile(h, g_out, frames * 4, &n, 0);
	}
	ExitProcess(0);
}
#endif
