/*
 * Copyright 2008 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdint.h>

#include "compiler.h"
#include "xf86.h"
#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) < 6
#include "xf86Resources.h"
#endif
#include "xf86PciInfo.h"
#include "xf86Cursor.h"
#include "xf86_OSproc.h"
#include "xf86xv.h"
#include "shadow.h"
#include "micmap.h"
#ifdef XSERVER_PCIACCESS
#include "pciaccess.h"
#endif
#include "fb.h"
#include "vgaHW.h"

#define hidden _X_HIDDEN

#define QXL_NAME		"compat_qxl"
#define QXL_DRIVER_NAME		"compat_qxl"
#define PCI_VENDOR_RED_HAT	0x1b36

#define PCI_CHIP_QXL_0100	0x0100

#pragma pack(push,1)

/* I/O port definitions */
enum {
    QXL_IO_NOTIFY_CMD,
    QXL_IO_NOTIFY_CURSOR,
    QXL_IO_UPDATE_AREA,
    QXL_IO_UPDATE_IRQ,
    QXL_IO_NOTIFY_OOM,
    QXL_IO_RESET,
    QXL_IO_SET_MODE,
    QXL_IO_LOG,
};

struct compat_qxl_mode {
    uint32_t id;
    uint32_t x_res;
    uint32_t y_res;
    uint32_t bits;
    uint32_t stride;
    uint32_t x_mili;
    uint32_t y_mili;
    uint32_t orientation;
};

typedef enum
{
    QXL_CMD_NOP,
    QXL_CMD_DRAW,
    QXL_CMD_UPDATE,
    QXL_CMD_CURSOR,
    QXL_CMD_MESSAGE
} compat_qxl_command_type;

struct compat_qxl_command {
    uint64_t data;
    uint32_t type;
    uint32_t pad;
};

struct compat_qxl_rect {
    uint32_t top;
    uint32_t left;
    uint32_t bottom;
    uint32_t right;
};

union compat_qxl_release_info {
    uint64_t id;
    uint64_t next;
};

struct compat_qxl_clip {
    uint32_t type;
    uint64_t address;
};

struct compat_qxl_point {
    int x;
    int y;
};

struct compat_qxl_pattern {
    uint64_t pat;
    struct compat_qxl_point pos;
};

typedef enum
{
    QXL_BRUSH_TYPE_NONE,
    QXL_BRUSH_TYPE_SOLID,
    QXL_BRUSH_TYPE_PATTERN
} compat_qxl_brush_type;

struct compat_qxl_brush {
    uint32_t type;
    union {
	uint32_t color;
	struct compat_qxl_pattern pattern;
    } u;
};

struct compat_qxl_mask {
    unsigned char flags;
    struct compat_qxl_point pos;
    uint64_t bitmap;
};

typedef enum {
    QXL_IMAGE_TYPE_BITMAP,
    QXL_IMAGE_TYPE_QUIC,
    QXL_IMAGE_TYPE_PNG,
    QXL_IMAGE_TYPE_LZ_PLT = 100,
    QXL_IMAGE_TYPE_LZ_RGB,
    QXL_IMAGE_TYPE_GLZ_RGB,
    QXL_IMAGE_TYPE_FROM_CACHE,
} compat_qxl_image_type;

typedef enum {
    QXL_IMAGE_CACHE = (1 << 0)
} compat_qxl_image_flags;

struct compat_qxl_image_descriptor
{
    uint64_t id;
    uint8_t type;
    uint8_t flags;
    uint32_t width;
    uint32_t height;
};

struct compat_qxl_data_chunk {
    uint32_t data_size;
    uint64_t prev_chunk;
    uint64_t next_chunk;
    uint8_t data[0];
};

typedef enum
{
    QXL_BITMAP_FMT_INVALID,
    QXL_BITMAP_FMT_1BIT_LE,
    QXL_BITMAP_FMT_1BIT_BE,
    QXL_BITMAP_FMT_4BIT_LE,
    QXL_BITMAP_FMT_4BIT_BE,
    QXL_BITMAP_FMT_8BIT,
    QXL_BITMAP_FMT_16BIT,
    QXL_BITMAP_FMT_24BIT,
    QXL_BITMAP_FMT_32BIT,
    QXL_BITMAP_FMT_RGBA,
} compat_qxl_bitmap_format;

typedef enum {
    QXL_BITMAP_PAL_CACHE_ME = (1 << 0),
    QXL_BITMAP_PAL_FROM_CACHE = (1 << 1),
    QXL_BITMAP_TOP_DOWN = (1 << 2),
} compat_qxl_bitmap_flags;

struct compat_qxl_bitmap {
    uint8_t format;
    uint8_t flags;		
    uint32_t x;			/* actually width */
    uint32_t y;			/* actually height */
    uint32_t stride;		/* in bytes */
    uint64_t palette;		/* Can be NULL */
    uint64_t data;		/* A compat_qxl_data_chunk that actually contains the data */
};

struct compat_qxl_image {
    struct compat_qxl_image_descriptor descriptor;
    union
    {
	struct compat_qxl_bitmap bitmap;
    } u;
};

struct compat_qxl_fill {
    struct compat_qxl_brush brush;
    unsigned short rop_descriptor;
    struct compat_qxl_mask mask;
};

struct compat_qxl_opaque {
    uint64_t src_bitmap;
    struct compat_qxl_rect src_area;
    struct compat_qxl_brush brush;
    unsigned short rop_descriptor;
    unsigned char scale_mode;
    struct compat_qxl_mask mask;
};

struct compat_qxl_copy {
    uint64_t src_bitmap;
    struct compat_qxl_rect src_area;
    unsigned short rop_descriptor;
    unsigned char scale_mode;
    struct compat_qxl_mask mask;
};

struct compat_qxl_transparent {
    uint64_t src_bitmap;
    struct compat_qxl_rect src_area;
    uint32_t src_color;
    uint32_t true_color;
};

struct compat_qxl_alpha_blend {
    unsigned char alpha;
    uint64_t src_bitmap;
    struct compat_qxl_rect src_area;
};

struct compat_qxl_copy_bits {
    struct compat_qxl_point src_pos;
};

struct compat_qxl_blend { /* same as copy */
    uint64_t src_bitmap;
    struct compat_qxl_rect src_area;
    unsigned short rop_descriptor;
    unsigned char scale_mode;
    struct compat_qxl_mask mask;
};

struct compat_qxl_rop3 {
    uint64_t src_bitmap;
    struct compat_qxl_rect src_area;
    struct compat_qxl_brush brush;
    unsigned char rop3;
    unsigned char scale_mode;
    struct compat_qxl_mask mask;
};

struct compat_qxl_line_attr {
    unsigned char flags;
    unsigned char join_style;
    unsigned char end_style;
    unsigned char style_nseg;
    int width;
    int miter_limit;
    uint64_t style;
};

struct compat_qxl_stroke {
    uint64_t path;
    struct compat_qxl_line_attr attr;
    struct compat_qxl_brush brush;
    unsigned short fore_mode;
    unsigned short back_mode;
};

struct compat_qxl_text {
    uint64_t str;
    struct compat_qxl_rect back_area;
    struct compat_qxl_brush fore_brush;
    struct compat_qxl_brush back_brush;
    unsigned short fore_mode;
    unsigned short back_mode;
};

struct compat_qxl_blackness {
    struct compat_qxl_mask mask;
};

struct compat_qxl_inverse {
    struct compat_qxl_mask mask;
};

struct compat_qxl_whiteness {
    struct compat_qxl_mask mask;
};

/* Effects */
typedef enum
{
    QXL_EFFECT_BLEND,
    QXL_EFFECT_OPAQUE,
    QXL_EFFECT_REVERT_ON_DUP,
    QXL_EFFECT_BLACKNESS_ON_DUP,
    QXL_EFFECT_WHITENESS_ON_DUP,
    QXL_EFFECT_NOP_ON_DUP,
    QXL_EFFECT_NOP,
    QXL_EFFECT_OPAQUE_BRUSH
} compat_qxl_effect_type;

typedef enum
{
    QXL_CLIP_TYPE_NONE,
    QXL_CLIP_TYPE_RECTS,
    QXL_CLIP_TYPE_PATH,
} compat_qxl_clip_type;

typedef enum {
    QXL_DRAW_NOP,
    QXL_DRAW_FILL,
    QXL_DRAW_OPAQUE,
    QXL_DRAW_COPY,
    QXL_COPY_BITS,
    QXL_DRAW_BLEND,
    QXL_DRAW_BLACKNESS,
    QXL_DRAW_WHITENESS,
    QXL_DRAW_INVERS,
    QXL_DRAW_ROP3,
    QXL_DRAW_STROKE,
    QXL_DRAW_TEXT,
    QXL_DRAW_TRANSPARENT,
    QXL_DRAW_ALPHA_BLEND,
} compat_qxl_draw_type;

struct compat_qxl_drawable {
    union compat_qxl_release_info release_info;
    unsigned char effect;
    unsigned char type;
    unsigned short bitmap_offset;
    struct compat_qxl_rect bitmap_area;
    struct compat_qxl_rect bbox;
    struct compat_qxl_clip clip;
    uint32_t mm_time;
    union {
	struct compat_qxl_fill fill;
	struct compat_qxl_opaque opaque;
	struct compat_qxl_copy copy;
	struct compat_qxl_transparent transparent;
	struct compat_qxl_alpha_blend alpha_blend;
	struct compat_qxl_copy_bits copy_bits;
	struct compat_qxl_blend blend;
	struct compat_qxl_rop3 rop3;
	struct compat_qxl_stroke stroke;
	struct compat_qxl_text text;
	struct compat_qxl_blackness blackness;
	struct compat_qxl_inverse inverse;
	struct compat_qxl_whiteness whiteness;
    } u;
};

struct compat_qxl_update_cmd {
    union compat_qxl_release_info release_info;
    struct compat_qxl_rect area;
    uint32_t update_id;
};

struct compat_qxl_point16 {
    int16_t x;
    int16_t y;
};

enum {
    QXL_CURSOR_SET,
    QXL_CURSOR_MOVE,
    QXL_CURSOR_HIDE,
    QXL_CURSOR_TRAIL,
};

#define QXL_CURSOR_DEVICE_DATA_SIZE 128

enum {
    CURSOR_TYPE_ALPHA,
    CURSOR_TYPE_MONO,
    CURSOR_TYPE_COLOR4,
    CURSOR_TYPE_COLOR8,
    CURSOR_TYPE_COLOR16,
    CURSOR_TYPE_COLOR24,
    CURSOR_TYPE_COLOR32,
};

struct compat_qxl_cursor_header {
    uint64_t unique;
    uint16_t type;
    uint16_t width;
    uint16_t height;
    uint16_t hot_spot_x;
    uint16_t hot_spot_y;
};

struct compat_qxl_cursor
{
    struct compat_qxl_cursor_header header;
    uint32_t data_size;
    struct compat_qxl_data_chunk chunk;
};

struct compat_qxl_cursor_cmd {
    union compat_qxl_release_info release_info;
    uint8_t type;
    union {
	struct {
	    struct compat_qxl_point16 position;
	    unsigned char visible;
	    uint64_t shape;
	} set;
	struct {
	    uint16_t length;
	    uint16_t frequency;
	} trail;
	struct compat_qxl_point16 position;
    } u;
    uint8_t device_data[QXL_CURSOR_DEVICE_DATA_SIZE];
};

struct compat_qxl_rom {
    uint32_t magic;
    uint32_t id;
    uint32_t update_id;
    uint32_t compression_level;
    uint32_t log_level;
    uint32_t mode;
    uint32_t modes_offset;
    uint32_t num_io_pages;
    uint32_t pages_offset;
    uint32_t draw_area_offset;
    uint32_t draw_area_size;
    uint32_t ram_header_offset;
    uint32_t mm_clock;
};

struct compat_qxl_ring_header {
    uint32_t num_items;
    uint32_t prod;
    uint32_t notify_on_prod;
    uint32_t cons;
    uint32_t notify_on_cons;
};

#define QXL_LOG_BUF_SIZE 4096

struct compat_qxl_ram_header {
    uint32_t magic;
    uint32_t int_pending;
    uint32_t int_mask;
    unsigned char log_buf[QXL_LOG_BUF_SIZE];
    struct compat_qxl_ring_header  cmd_ring_hdr;
    struct compat_qxl_command	    cmd_ring[32];
    struct compat_qxl_ring_header  cursor_ring_hdr;
    struct compat_qxl_command	    cursor_ring[32];
    struct compat_qxl_ring_header  release_ring_hdr;
    uint64_t		    release_ring[8];
    struct compat_qxl_rect	    update_area;
};

#pragma pack(pop)

typedef struct _compat_qxl_screen_t compat_qxl_screen_t;

struct _compat_qxl_screen_t
{
    /* These are the names QXL uses */
    void *			ram;	/* Video RAM */
    void *			ram_physical;
    void *			vram;	/* Command RAM */
    struct compat_qxl_rom *		rom;    /* Parameter RAM */
    
    struct compat_qxl_ring *		command_ring;
    struct compat_qxl_ring *		cursor_ring;
    struct compat_qxl_ring *		release_ring;
    
    int				num_modes;
    struct compat_qxl_mode *		modes;
    int				io_base;
    int				draw_area_offset;
    int				draw_area_size;

    void *			fb;
    int				bytes_per_pixel;

    struct compat_qxl_mem *		mem;	/* Context for compat_qxl_alloc/free */
    
    EntityInfoPtr		entity;

    void *			io_pages;
    void *			io_pages_physical;
    
#ifdef XSERVER_LIBPCIACCESS
    struct pci_device *		pci;
#else
    pciVideoPtr			pci;
    PCITAG			pci_tag;
#endif
    vgaRegRec                   vgaRegs;

    CreateScreenResourcesProcPtr create_screen_resources;
    CloseScreenProcPtr		close_screen;
    CreateGCProcPtr		create_gc;
#if 0
    PaintWindowProcPtr		paint_window_background;
    PaintWindowProcPtr		paint_window_border;
#endif
    CopyWindowProcPtr		copy_window;
    
    DamagePtr			damage;
    RegionRec			pending_copy;
    RegionRec			to_be_sent;
    
    int16_t			cur_x;
    int16_t			cur_y;
    int16_t			hot_x;
    int16_t			hot_y;
    
    ScrnInfoPtr			pScrn;
};

static inline uint64_t
physical_address (compat_qxl_screen_t *compat_qxl, void *virtual)
{
    return (uint64_t) ((unsigned long)virtual + (((unsigned long)compat_qxl->ram_physical - (unsigned long)compat_qxl->ram)));
}

static inline void *
virtual_address (compat_qxl_screen_t *compat_qxl, void *physical)
{
    return (void *) ((unsigned long)physical + ((unsigned long)compat_qxl->ram - (unsigned long)compat_qxl->ram_physical));
}

static inline void *
u64_to_pointer (uint64_t u)
{
    return (void *)(unsigned long)u;
}

static inline uint64_t
pointer_to_u64 (void *p)
{
    return (uint64_t)(unsigned long)p;
}

struct compat_qxl_ring;

/*
 * HW cursor
 */
void              compat_qxl_cursor_init        (ScreenPtr               pScreen);



/*
 * Rings
 */
struct compat_qxl_ring * compat_qxl_ring_create      (struct compat_qxl_ring_header *header,
					int                     element_size,
					int                     n_elements,
					int                     prod_notify);
void              compat_qxl_ring_push        (struct compat_qxl_ring        *ring,
					const void             *element);
Bool              compat_qxl_ring_pop         (struct compat_qxl_ring        *ring,
					void                   *element);
void              compat_qxl_ring_wait_idle   (struct compat_qxl_ring        *ring);



/*
 * Images
 */
struct compat_qxl_image *compat_qxl_image_create     (compat_qxl_screen_t           *compat_qxl,
					const uint8_t          *data,
					int                     x,
					int                     y,
					int                     width,
					int                     height,
					int                     stride);
void              compat_qxl_image_destroy    (compat_qxl_screen_t           *compat_qxl,
					struct compat_qxl_image       *image);
void		  compat_qxl_drop_image_cache (compat_qxl_screen_t	       *compat_qxl);


/*
 * Malloc
 */
struct compat_qxl_mem *  compat_qxl_mem_create       (void                   *base,
					unsigned long           n_bytes);
void              compat_qxl_mem_dump_stats   (struct compat_qxl_mem         *mem,
					const char             *header);
void *            compat_qxl_alloc            (struct compat_qxl_mem         *mem,
					unsigned long           n_bytes);
void              compat_qxl_free             (struct compat_qxl_mem         *mem,
					void                   *d);
void              compat_qxl_mem_free_all     (struct compat_qxl_mem         *mem);
void *            compat_qxl_allocnf          (compat_qxl_screen_t           *compat_qxl,
					unsigned long           size);


