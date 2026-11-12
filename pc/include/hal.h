/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * hal.h — Hardware Abstraction Layer for Tokimeki Memorial PC Port
 *
 * Replaces PsyQ SDK functions with PC-native implementations.
 * Game code includes this instead of psyq/ headers.
 */
#ifndef HAL_H
#define HAL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GPU / Display ---- */

#ifndef RECT_DEFINED
typedef struct { s16 x, y, w, h; } RECT;
#define RECT_DEFINED
#endif

typedef struct {
    /* Match PS1 DRAWENV layout exactly (offsets matter for game code) */
    RECT clip;          /* 0x00: clip area */
    s16 ofs[2];         /* 0x08: drawing offset */
    RECT tw;            /* 0x0C: texture window */
    u16 tpage;          /* 0x14: texture page */
    u8 dtd;             /* 0x16: dither flag */
    u8 dfe;             /* 0x17: draw on display area */
    u8 isbg;            /* 0x18: auto-clear */
    u8 r0, g0, b0;      /* 0x19: background color */
    u32 dr_env[16];      /* 0x1C: 4-byte tag plus 15 command words */
} DRAWENV;              /* size = 0x5C */

typedef struct {
    RECT disp;          /* 0x00: VRAM display rectangle */
    RECT screen;        /* 0x08: screen position/range */
    u8 isinter;         /* 0x10: interlace */
    u8 isrgb24;         /* 0x11: packed RGB24 scanout */
    u8 pad0, pad1;
} DISPENV;              /* size = 0x14 */

void ResetGraph(s32 mode);
DRAWENV *SetDefDrawEnv(DRAWENV *env, s32 x, s32 y, s32 w, s32 h);
DISPENV *SetDefDispEnv(DISPENV *env, s32 x, s32 y, s32 w, s32 h);
DRAWENV *PutDrawEnv(DRAWENV *env);
DISPENV *PutDispEnv(DISPENV *env);
void SetDispMask(s32 mask);
typedef void (*GpuSyncCallback)(void);
GpuSyncCallback DrawSyncCallback(GpuSyncCallback callback);
s32 DrawSync(s32 mode);
s32 VSync(s32 mode);
void ClearImage(RECT *rect, u8 r, u8 g, u8 b);
void LoadImage(RECT *rect, u32 *data);
int pc_ptr_is_mapped(const void *p, unsigned long bytes);   /* pc/hal/ptr_validate.c */
void *hal_ps1_ptr(const void *p);                     /* fold a sign-extended PS1 address back into the mapped window */
void StoreImage(RECT *rect, u32 *data);
void MoveImage(RECT *src, s32 dx, s32 dy);
void ClearOTag(u32 *ot, s32 n);
void DrawOTag(u32 *ot);

/* VRAM region bpp tracking for NBN sprites */
void gpu_set_region_bpp(s32 tpage_x, s32 bpp);
s32 gpu_get_region_bpp(s32 tpage_x);

/* ---- GTE / 3D Math ---- */

typedef struct { s16 m[3][3]; s32 t[3]; } MATRIX;
typedef struct { s32 vx, vy, vz, pad; } VECTOR;
typedef struct { s16 vx, vy, vz, pad; } SVECTOR;
typedef struct { u8 r, g, b, cd; } CVECTOR;
typedef struct { s16 vx, vy; } DVECTOR;

/* Core GTE initialization */
void InitGeom(void);
void SetGeomOffset(s32 ofx, s32 ofy);
void SetGeomScreen(s32 h);
void ReadGeomOffset(s32 *ofx, s32 *ofy);
s32 ReadGeomScreen(void);

/* Matrix operations */
void SetRotMatrix(MATRIX *m);
void SetTransMatrix(MATRIX *m);
void SetLightMatrix(MATRIX *m);
void SetColorMatrix(MATRIX *m);
void ReadRotMatrix(MATRIX *m);
void ReadLightMatrix(MATRIX *m);
void ReadColorMatrix(MATRIX *m);
void PushMatrix(void);
void PopMatrix(void);

MATRIX *MulMatrix0(MATRIX *m0, MATRIX *m1, MATRIX *m2);
MATRIX *MulMatrix(MATRIX *m0, MATRIX *m1);
MATRIX *MulMatrix2(MATRIX *m0, MATRIX *m1);
MATRIX *MulRotMatrix0(MATRIX *m0, MATRIX *m1);
MATRIX *MulRotMatrix(MATRIX *m0);
MATRIX *SetMulMatrix(MATRIX *m0, MATRIX *m1);
MATRIX *SetMulRotMatrix(MATRIX *m0);
MATRIX *RotMatrix(SVECTOR *r, MATRIX *m);
MATRIX *TransMatrix(MATRIX *m, VECTOR *v);
MATRIX *ScaleMatrix(MATRIX *m, VECTOR *v);
MATRIX *ScaleMatrixL(MATRIX *m, VECTOR *v);
MATRIX *TransposeMatrix(MATRIX *m0, MATRIX *m1);
MATRIX *CompMatrix(MATRIX *m0, MATRIX *m1, MATRIX *m2);
MATRIX *CompMatrixLV(MATRIX *m0, MATRIX *m1, MATRIX *m2);
int IsIdMatrix(MATRIX *m);

/* Vector operations */
VECTOR *ApplyMatrix(MATRIX *m, SVECTOR *v0, VECTOR *v1);
VECTOR *ApplyMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1);
SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v0, SVECTOR *v1);
VECTOR *ApplyRotMatrix(SVECTOR *v0, VECTOR *v1);
VECTOR *ApplyRotMatrixLV(VECTOR *v0, VECTOR *v1);
VECTOR *ApplyTransposeMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1);
VECTOR *Square12(VECTOR *v0, VECTOR *v1);
VECTOR *Square0(VECTOR *v0, VECTOR *v1);
void OuterProduct12(VECTOR *v0, VECTOR *v1, VECTOR *v2);
void OuterProduct0(VECTOR *v0, VECTOR *v1, VECTOR *v2);
s32 VectorNormal(VECTOR *v0, VECTOR *v1);
s32 VectorNormalS(VECTOR *v0, SVECTOR *v1);

/* Transform operations */
void RotTrans(SVECTOR *v0, VECTOR *v1, s32 *flag);
void RotTransSV(SVECTOR *v0, SVECTOR *v1, s32 *flag);
s32 RotTransPers(SVECTOR *v0, s32 *sxy, s32 *p, s32 *flag);
s32 RotTransPers3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                  s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *flag);
s32 RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3,
                  s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3, s32 *p, s32 *flag);

/* Clipping and depth */
s32 NormalClip(s32 sxy0, s32 sxy1, s32 sxy2);
s32 AverageZ3(s32 sz0, s32 sz1, s32 sz2);
s32 AverageZ4(s32 sz0, s32 sz1, s32 sz2, s32 sz3);
s32 RotAverage3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *flag);
s32 RotAverage4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3,
                s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3, s32 *p, s32 *flag);
s32 RotNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
              s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *otz, s32 *flag);
s32 RotAverageNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                     s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *otz, s32 *flag);
s32 RotAverageNclip4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3,
                     s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3, s32 *p, s32 *otz, s32 *flag);

/* FIFO reads */
void ReadSZfifo3(s32 *sz0, s32 *sz1, s32 *sz2);
void ReadSZfifo4(s32 *szx, s32 *sz0, s32 *sz1, s32 *sz2);
void ReadSXSYfifo(s32 *sxy0, s32 *sxy1, s32 *sxy2);
void ReadRGBfifo(CVECTOR *v0, CVECTOR *v1, CVECTOR *v2);

/* Color operations */
void SetRGBcd(CVECTOR *v);
void SetBackColor(s32 rbk, s32 gbk, s32 bbk);
void SetFarColor(s32 rfc, s32 gfc, s32 bfc);
void LocalLight(SVECTOR *v0, VECTOR *v1);
void LightColor(VECTOR *v0, VECTOR *v1);
void NormalColor(SVECTOR *v0, CVECTOR *v1);
void NormalColor3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2,
                  CVECTOR *c0, CVECTOR *c1, CVECTOR *c2);
void DpqColor(CVECTOR *v0, s32 p, CVECTOR *v1);
void Intpl(VECTOR *v0, s32 p, CVECTOR *v1);
void LoadAverage12(VECTOR *v0, VECTOR *v1, s32 p0, s32 p1, VECTOR *v2);
void LoadAverageShort12(SVECTOR *v0, SVECTOR *v1, s32 p0, s32 p1, SVECTOR *v2);

/* Math utilities */
s32 SquareRoot0(s32 a);
s32 SquareRoot12(s32 a);
void InvSquareRoot(s32 a, s32 *b, s32 *c);
s32 Lzc(s32 data);
s32 rsin(s32 a);
s32 rcos(s32 a);
s32 csin(s32 a);
s32 ccos(s32 a);
s32 ratan2(s32 y, s32 x);

/* OTZ conversions */
s32 otz2p(s32 otz, s32 projection);
s32 p2otz(s32 p, s32 projection);

/* ---- Sound ---- */

void SpuInit(void);
void SpuQuit(void);
s16 SsVabOpenHead(u8 *addr, s16 vab_id);
/* libsnd's sequence API and VAB transfer are the matched libsnd C; the HAL
 * owns the sequencer tick (_SsStart/SsStart/SsStart2) and the SPU register
 * block those functions write (spu_mmio_sync, pc/hal/spu.c). */
void SsStart(void);
void SsStart2(void);
void spu_mmio_sync(void);
void audio_seq_stop(void);              /* PC-only SEQ player (sound test) */

/* ---- CD / Filesystem ---- */

s32 CdInit(void);
s32 cd_hal_boot_handoff(void);
s32 CdControl(u8 cmd, u8 *param, u8 *result);
s32 CdRead(s32 sectors, u32 *buf, s32 mode);
s32 CdSync(s32 mode, u8 *result);

/* HAL-specific: load file from assets directory */
s32 hal_load_file(const char *path, void *buf, s32 max_size);

/* ---- Input ---- */

void PadInit(s32 mode);
u32 PadRead(s32 port);

/* ---- System ---- */

void hal_init(const char *asset_dir);
void hal_shutdown(void);
void hal_frame_begin(void);
void hal_frame_end(void);
int hal_is_running(void);
void hal_poll_quit(void);
/* A modal error dialog (SDL message box; the player's window may be gone). */
void hal_show_error(const char *title, const char *msg);
void hal_request_quit(void);
/* Input HAL (pc/hal/pad.c): the once-per-tick libpad buffer refresh. */
void pad_update_buffers(void);

/* ---- Asset System ---- */

void assets_init(const char *base_path);
s32 assets_resolve_path(const char *ps1_path, char *out, s32 max_len);
u8 *assets_load_file(const char *ps1_path, s32 *out_size);
s32 assets_load_to_buffer(const char *ps1_path, void *buf, s32 max_size);

/* ---- TIM Loader ---- */

s32 tim_load_file(const char *path);
s32 tim_load_asset(const char *asset_name);

/* ---- Debug Tools ---- */

void debug_init(void);
void debug_shutdown(void);
void debug_log(const char *fmt, ...);
void debug_frame(void);
int debug_overlay_is_active(void);
void debug_screenshot(const char *path);

/* ---- Memory Card (PSX BIOS file API -> ./saves/) ---- */

void memcard_init(void);
void memcard_card_init(void);
void memcard_card_start(void);
void memcard_vblank_tick(void);
u64 memcard_next_event(void);
void memcard_complete_event(void);
long memcard_card_auto(long enabled);
void memcard_new_card(void);
long memcard_card_read(long chan, long sector, u8 *buffer);
long memcard_card_write(long chan, long sector, const u8 *buffer);
int  memcard_bu_init(void);
long memcard_card_load(long chan);
long memcard_card_info(long chan);
long memcard_card_status(long chan);
int  memcard_file_open(const char *filename, u32 accessmode);
int  memcard_file_delete(const char *filename);
int  memcard_file_read(int fd, void *buf, int nbytes);
int  memcard_file_write(int fd, const void *buf, int nbytes);
int  memcard_file_close(int fd);
int  memcard_file_seek(int fd, int offset, int mode);
void memcard_mark_ready(void);  /* sets D_800FF869 — memcard-OK gate */

/* ---- Asset Mapping System ---- */

int asset_map_init(void);
const char *asset_map_get_path(const char *id);
int asset_map_get_vram(const char *id, s32 *x, s32 *y, s32 *w, s32 *h);
int asset_map_get_clut(const char *id, s32 *x, s32 *y);
void asset_map_mark_loaded(const char *id);
int asset_map_is_loaded(const char *id);
void asset_map_dump(void);

/* ---- Overlay System ---- */
void overlay_init(void);
void overlay_stub_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_H */
