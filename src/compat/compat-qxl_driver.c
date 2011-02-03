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

/** \file compat_qxl_driver.c
 * \author Adam Jackson <ajax@redhat.com>
 *
 * This is compat_qxl, a driver for the Qumranet paravirtualized graphics device
 * in qemu.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include "compat-qxl.h"
#include "assert.h"

#define CHECK_POINT()

static int
garbage_collect (compat_qxl_screen_t *compat_qxl)
{
    uint64_t id;
    int i = 0;
    
    while (compat_qxl_ring_pop (compat_qxl->release_ring, &id))
    {
	while (id)
	{
	    /* We assume that there the two low bits of a pointer are
	     * available. If the low one is set, then the command in
	     * question is a cursor command
	     */
#define POINTER_MASK ((1 << 2) - 1)
	    
	    union compat_qxl_release_info *info = u64_to_pointer (id & ~POINTER_MASK);
	    struct compat_qxl_cursor_cmd *cmd = (struct compat_qxl_cursor_cmd *)info;
	    struct compat_qxl_drawable *drawable = (struct compat_qxl_drawable *)info;
	    int is_cursor = FALSE;

	    if ((id & POINTER_MASK) == 1)
		is_cursor = TRUE;

	    if (is_cursor && cmd->type == QXL_CURSOR_SET)
	    {
		struct compat_qxl_cursor *cursor = (void *)virtual_address (
		    compat_qxl, u64_to_pointer (cmd->u.set.shape));

		compat_qxl_free (compat_qxl->mem, cursor);
	    }
	    else if (!is_cursor && drawable->type == QXL_DRAW_COPY)
	    {
		struct compat_qxl_image *image = virtual_address (
		    compat_qxl, u64_to_pointer (drawable->u.copy.src_bitmap));

		compat_qxl_image_destroy (compat_qxl, image);
	    }
	    
	    id = info->next;
	    
	    compat_qxl_free (compat_qxl->mem, info);
	}
    }

    return i > 0;
}

static void
compat_qxl_usleep (int useconds)
{
    struct timespec t;

    t.tv_sec = useconds / 1000000;
    t.tv_nsec = (useconds - (t.tv_sec * 1000000)) * 1000;

    errno = 0;
    while (nanosleep (&t, &t) == -1 && errno == EINTR)
	;
    
}

#if 0
static void
push_update_area (compat_qxl_screen_t *compat_qxl, const struct compat_qxl_rect *area)
{
    struct compat_qxl_update_cmd *update = compat_qxl_allocnf (compat_qxl, sizeof *update);
    struct compat_qxl_command cmd;

    update->release_info.id = (uint64_t)update;
    update->area = *area;
    update->update_id = 0;

    cmd.type = QXL_CMD_UDPATE;
    cmd.data = physical_address (compat_qxl, update);

    compat_qxl_ring_push (compat_qxl->command_ring, &cmd);
}
#endif

void *
compat_qxl_allocnf (compat_qxl_screen_t *compat_qxl, unsigned long size)
{
    void *result;
    int n_attempts = 0;
    static int nth_oom = 1;

    garbage_collect (compat_qxl);
    
    while (!(result = compat_qxl_alloc (compat_qxl->mem, size)))
    {
	struct compat_qxl_ram_header *ram_header = (void *)((unsigned long)compat_qxl->ram +
						     compat_qxl->rom->ram_header_offset);
	
	/* Rather than go out of memory, we simply tell the
	 * device to dump everything
	 */
	ram_header->update_area.top = 0;
	ram_header->update_area.bottom = 1280;
	ram_header->update_area.left = 0;
	ram_header->update_area.right = 800;
	
	outb (compat_qxl->io_base + QXL_IO_UPDATE_AREA, 0);
	
 	ErrorF ("eliminated memory (%d)\n", nth_oom++);

	outb (compat_qxl->io_base + QXL_IO_NOTIFY_OOM, 0);

	compat_qxl_usleep (10000);
	
	if (garbage_collect (compat_qxl))
	{
	    n_attempts = 0;
	}
	else if (++n_attempts == 1000)
	{
	    compat_qxl_mem_dump_stats (compat_qxl->mem, "Out of mem - stats\n");
	    
	    fprintf (stderr, "Out of memory\n");
	    exit (1);
	}
    }

    return result;
}

static Bool
compat_qxl_blank_screen(ScreenPtr pScreen, int mode)
{
    return TRUE;
}

static void
compat_qxl_unmap_memory(compat_qxl_screen_t *compat_qxl, int scrnIndex)
{
#ifdef XSERVER_LIBPCIACCESS
    if (compat_qxl->ram)
	pci_device_unmap_range(compat_qxl->pci, compat_qxl->ram, compat_qxl->pci->regions[0].size);
    if (compat_qxl->vram)
	pci_device_unmap_range(compat_qxl->pci, compat_qxl->vram, compat_qxl->pci->regions[1].size);
    if (compat_qxl->rom)
	pci_device_unmap_range(compat_qxl->pci, compat_qxl->rom, compat_qxl->pci->regions[2].size);
#else
    if (compat_qxl->ram)
	xf86UnMapVidMem(scrnIndex, compat_qxl->ram, (1 << compat_qxl->pci->size[0]));
    if (compat_qxl->vram)
	xf86UnMapVidMem(scrnIndex, compat_qxl->vram, (1 << compat_qxl->pci->size[1]));
    if (compat_qxl->rom)
	xf86UnMapVidMem(scrnIndex, compat_qxl->rom, (1 << compat_qxl->pci->size[2]));
#endif

    compat_qxl->ram = compat_qxl->ram_physical = compat_qxl->vram = compat_qxl->rom = NULL;

    compat_qxl->num_modes = 0;
    compat_qxl->modes = NULL;
}

static Bool
compat_qxl_map_memory(compat_qxl_screen_t *compat_qxl, int scrnIndex)
{
#ifdef XSERVER_LIBPCIACCESS
    pci_device_map_range(compat_qxl->pci, compat_qxl->pci->regions[0].base_addr, 
			 compat_qxl->pci->regions[0].size,
			 PCI_DEV_MAP_FLAG_WRITABLE | PCI_DEV_MAP_FLAG_WRITE_COMBINE,
			 &compat_qxl->ram);
    compat_qxl->ram_physical = u64_to_pointer (compat_qxl->pci->regions[0].base_addr);

    pci_device_map_range(compat_qxl->pci, compat_qxl->pci->regions[1].base_addr, 
			 compat_qxl->pci->regions[1].size,
			 PCI_DEV_MAP_FLAG_WRITABLE,
			 &compat_qxl->vram);

    pci_device_map_range(compat_qxl->pci, compat_qxl->pci->regions[2].base_addr, 
			 compat_qxl->pci->regions[2].size, 0,
			 (void **)&compat_qxl->rom);

    compat_qxl->io_base = compat_qxl->pci->regions[3].base_addr;
#else
    compat_qxl->ram = xf86MapPciMem(scrnIndex, VIDMEM_FRAMEBUFFER,
			     compat_qxl->pci_tag, compat_qxl->pci->memBase[0],
			     (1 << compat_qxl->pci->size[0]));
    compat_qxl->ram_physical = (void *)compat_qxl->pci->memBase[0];
    
    compat_qxl->vram = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			      compat_qxl->pci_tag, compat_qxl->pci->memBase[1],
			      (1 << compat_qxl->pci->size[1]));
    
    compat_qxl->rom = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			     compat_qxl->pci_tag, compat_qxl->pci->memBase[2],
			     (1 << compat_qxl->pci->size[2]));
    
    compat_qxl->io_base = compat_qxl->pci->ioBase[3];
#endif
    if (!compat_qxl->ram || !compat_qxl->vram || !compat_qxl->rom)
	return FALSE;

    xf86DrvMsg(scrnIndex, X_INFO, "ram at %p; vram at %p; rom at %p\n",
	       compat_qxl->ram, compat_qxl->vram, compat_qxl->rom);

    compat_qxl->num_modes = *(uint32_t *)((uint8_t *)compat_qxl->rom + compat_qxl->rom->modes_offset);
    compat_qxl->modes = (struct compat_qxl_mode *)(((uint8_t *)compat_qxl->rom) + compat_qxl->rom->modes_offset + 4);

    return TRUE;
}

static void
compat_qxl_save_state(ScrnInfoPtr pScrn)
{
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;

    vgaHWSaveFonts(pScrn, &compat_qxl->vgaRegs);
}

static void
compat_qxl_restore_state(ScrnInfoPtr pScrn)
{
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;

    vgaHWRestoreFonts(pScrn, &compat_qxl->vgaRegs);
}

static Bool
compat_qxl_close_screen(int scrnIndex, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;

    if (pScrn->vtSema) {
        compat_qxl_restore_state(pScrn);
	compat_qxl_unmap_memory(compat_qxl, scrnIndex);
    }
    pScrn->vtSema = FALSE;

    xfree(compat_qxl->fb);

    pScreen->CreateScreenResources = compat_qxl->create_screen_resources;
    pScreen->CloseScreen = compat_qxl->close_screen;

    return pScreen->CloseScreen(scrnIndex, pScreen);
}

static Bool
compat_qxl_switch_mode(int scrnIndex, DisplayModePtr p, int flags)
{
    compat_qxl_screen_t *compat_qxl = xf86Screens[scrnIndex]->driverPrivate;
    int mode_index = (int)(unsigned long)p->Private;
    struct compat_qxl_mode *m = compat_qxl->modes + mode_index;
    ScreenPtr pScreen = compat_qxl->pScrn->pScreen;

    if (!m)
	return FALSE;

    /* if (debug) */
    xf86DrvMsg (scrnIndex, X_INFO, "Setting mode %d (%d x %d) (%d x %d) %p\n",
		m->id, m->x_res, m->y_res, p->HDisplay, p->VDisplay, p);

    outb(compat_qxl->io_base + QXL_IO_RESET, 0);
    
    outb(compat_qxl->io_base + QXL_IO_SET_MODE, m->id);

    compat_qxl->bytes_per_pixel = (compat_qxl->pScrn->bitsPerPixel + 7) / 8;

    /* If this happens out of ScreenInit, we won't have a screen yet. In that
     * case createScreenResources will make things right.
     */
    if (pScreen)
    {
	PixmapPtr pPixmap = pScreen->GetScreenPixmap(pScreen);

	if (pPixmap)
	{
	    pScreen->ModifyPixmapHeader(
		pPixmap,
		m->x_res, m->y_res,
		-1, -1,
		compat_qxl->pScrn->displayWidth * compat_qxl->bytes_per_pixel,
		NULL);
	}
    }
    
    if (compat_qxl->mem)
    {
	compat_qxl_mem_free_all (compat_qxl->mem);
	compat_qxl_drop_image_cache (compat_qxl);
    }

    
    return TRUE;
}

static void
push_drawable (compat_qxl_screen_t *compat_qxl, struct compat_qxl_drawable *drawable)
{
    struct compat_qxl_command cmd;

    /* When someone runs "init 3", the device will be 
     * switched into VGA mode and there is nothing we
     * can do about it. We get no notification.
     * 
     * However, if commands are submitted when the device
     * is in VGA mode, they will be queued up, and then
     * the next time a mode set set, an assertion in the
     * device will take down the entire virtual machine.
     * 
     * The author of the QXL device is opposed to this
     * for reasons I don't understand.
     */
    if (compat_qxl->rom->mode != ~0)
    {
	cmd.type = QXL_CMD_DRAW;
	cmd.data = physical_address (compat_qxl, drawable);
	    
	compat_qxl_ring_push (compat_qxl->command_ring, &cmd);
    }
}

static struct compat_qxl_drawable *
make_drawable (compat_qxl_screen_t *compat_qxl, uint8_t type,
	       const struct compat_qxl_rect *rect
	       /* , pRegion clip */)
{
    struct compat_qxl_drawable *drawable;

    CHECK_POINT();
    
    drawable = compat_qxl_allocnf (compat_qxl, sizeof *drawable);

    CHECK_POINT();

    drawable->release_info.id = pointer_to_u64 (drawable);

    drawable->type = type;

    drawable->effect = QXL_EFFECT_OPAQUE;
    drawable->bitmap_offset = 0;
    drawable->bitmap_area.top = 0;
    drawable->bitmap_area.left = 0;
    drawable->bitmap_area.bottom = 0;
    drawable->bitmap_area.right = 0;
    /* FIXME: add clipping */
    drawable->clip.type = QXL_CLIP_TYPE_NONE;

    if (rect)
	drawable->bbox = *rect;

    drawable->mm_time = compat_qxl->rom->mm_clock;

    CHECK_POINT();
    
    return drawable;
}

enum ROPDescriptor {
    ROPD_INVERS_SRC = (1 << 0),
    ROPD_INVERS_BRUSH = (1 << 1),
    ROPD_INVERS_DEST = (1 << 2),
    ROPD_OP_PUT = (1 << 3),
    ROPD_OP_OR = (1 << 4),
    ROPD_OP_AND = (1 << 5),
    ROPD_OP_XOR = (1 << 6),
    ROPD_OP_BLACKNESS = (1 << 7),
    ROPD_OP_WHITENESS = (1 << 8),
    ROPD_OP_INVERS = (1 << 9),
    ROPD_INVERS_RES = (1 <<10),
};

static void
undamage_box (compat_qxl_screen_t *compat_qxl, const struct compat_qxl_rect *rect)
{
    RegionRec region;
    BoxRec box;

    box.x1 = rect->left;
    box.y1 = rect->top;
    box.x2 = rect->right;
    box.y2 = rect->bottom;

    REGION_INIT (compat_qxl->pScrn->pScreen, &region, &box, 0);

    REGION_SUBTRACT (compat_qxl->pScrn->pScreen, &(compat_qxl->pending_copy), &(compat_qxl->pending_copy), &region);

    REGION_EMPTY (compat_qxl->pScrn->pScreen, &(compat_qxl->pending_copy));
}

static void
clear_pending_damage (compat_qxl_screen_t *compat_qxl)
{
    REGION_EMPTY (compat_qxl->pScrn->pScreen, &(compat_qxl->pending_copy));
}

static void
submit_fill (compat_qxl_screen_t *compat_qxl, const struct compat_qxl_rect *rect, uint32_t color)
{
    struct compat_qxl_drawable *drawable;

    CHECK_POINT();
    
    drawable = make_drawable (compat_qxl, QXL_DRAW_FILL, rect);

    CHECK_POINT();

    drawable->u.fill.brush.type = QXL_BRUSH_TYPE_SOLID;
    drawable->u.fill.brush.u.color = color;
    drawable->u.fill.rop_descriptor = ROPD_OP_PUT;
    drawable->u.fill.mask.flags = 0;
    drawable->u.fill.mask.pos.x = 0;
    drawable->u.fill.mask.pos.y = 0;
    drawable->u.fill.mask.bitmap = 0;

    push_drawable (compat_qxl, drawable);

    undamage_box (compat_qxl, rect);
}

static void
translate_rect (struct compat_qxl_rect *rect)
{
    rect->right -= rect->left;
    rect->bottom -= rect->top;
    rect->left = rect->top = 0;
}

static void
submit_copy (compat_qxl_screen_t *compat_qxl, const struct compat_qxl_rect *rect)
{
    struct compat_qxl_drawable *drawable;
    ScrnInfoPtr pScrn = compat_qxl->pScrn;

    if (rect->left == rect->right ||
	rect->top == rect->bottom)
    {
	/* Empty rectangle */
	return ;
    }
    
    drawable = make_drawable (compat_qxl, QXL_DRAW_COPY, rect);

    drawable->u.copy.src_bitmap = physical_address (
	compat_qxl, compat_qxl_image_create (compat_qxl, compat_qxl->fb, rect->left, rect->top,
			       rect->right - rect->left,
			       rect->bottom - rect->top,
			       pScrn->displayWidth * compat_qxl->bytes_per_pixel));
    drawable->u.copy.src_area = *rect;
    translate_rect (&drawable->u.copy.src_area);
    drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
    drawable->u.copy.scale_mode = 0;
    drawable->u.copy.mask.flags = 0;
    drawable->u.copy.mask.pos.x = 0;
    drawable->u.copy.mask.pos.y = 0;
    drawable->u.copy.mask.bitmap = 0;

    push_drawable (compat_qxl, drawable);
}

static void
print_region (const char *header, RegionPtr pRegion)
{
    int nbox = REGION_NUM_RECTS (pRegion);
    BoxPtr pbox = REGION_RECTS (pRegion);

    ErrorF ("%s \n", header);
    
    while (nbox--)
    {
	ErrorF ("   %d %d %d %d (size: %d %d)\n",
		pbox->x1, pbox->y1, pbox->x2, pbox->y2,
		pbox->x2 - pbox->x1, pbox->y2 - pbox->y1);

	pbox++;
    }
}

static void
accept_damage (compat_qxl_screen_t *compat_qxl)
{
    REGION_UNION (compat_qxl->pScrn->pScreen, &(compat_qxl->to_be_sent), &(compat_qxl->to_be_sent), 
		  &(compat_qxl->pending_copy));

    REGION_EMPTY (compat_qxl->pScrn->pScreen, &(compat_qxl->pending_copy));
}

static void
compat_qxl_send_copies (compat_qxl_screen_t *compat_qxl)
{
    BoxPtr pBox;
    int nbox;

    nbox = REGION_NUM_RECTS (&compat_qxl->to_be_sent);
    pBox = REGION_RECTS (&compat_qxl->to_be_sent);

/*      if (REGION_NUM_RECTS (&compat_qxl->to_be_sent) > 0)  */
/*        	print_region ("send bits", &compat_qxl->to_be_sent); */
    
    while (nbox--)
    {
	struct compat_qxl_rect qrect;

	qrect.top = pBox->y1;
	qrect.left = pBox->x1;
	qrect.bottom = pBox->y2;
	qrect.right = pBox->x2;
	
	submit_copy (compat_qxl, &qrect);

	pBox++;
    }

    REGION_EMPTY(compat_qxl->pScrn->pScreen, &compat_qxl->to_be_sent);
}

static void
paint_shadow (compat_qxl_screen_t *compat_qxl)
{
    struct compat_qxl_rect qrect;

    qrect.top = 0;
    qrect.bottom = 1200;
    qrect.left = 0;
    qrect.right = 1600;

    submit_copy (compat_qxl, &qrect);
}

static void
compat_qxl_sanity_check (compat_qxl_screen_t *compat_qxl)
{
    /* read the mode back from the rom */
    if (!compat_qxl->rom || !compat_qxl->pScrn)
	return;

    if (compat_qxl->rom->mode == ~0) 
    {
 	ErrorF("QXL device jumped back to VGA mode - resetting mode\n");
 	compat_qxl_switch_mode(compat_qxl->pScrn->scrnIndex, compat_qxl->pScrn->currentMode, 0);
    }
}

static void
compat_qxl_block_handler (pointer data, OSTimePtr pTimeout, pointer pRead)
{
    compat_qxl_screen_t *compat_qxl = (compat_qxl_screen_t *) data;

    if (!compat_qxl->pScrn->vtSema)
        return;

    compat_qxl_sanity_check(compat_qxl);

    accept_damage (compat_qxl);

    compat_qxl_send_copies (compat_qxl);
}

static void
compat_qxl_wakeup_handler (pointer data, int i, pointer LastSelectMask)
{
}

/* Damage Handling
 * 
 * When something is drawn, X first generates a damage callback, then
 * it calls the GC function to actually draw it. In most cases, we want
 * to simply draw into the shadow framebuffer, then submit a copy to the
 * device, but when the operation is hardware accelerated, we don't want
 * to submit the copy. So, damage is first accumulated into 'pending_copy',
 * then if we accelerated the operation, that damage is deleted. 
 *
 * If we _didn't_ accelerate, we need to union the pending_copy damage 
 * onto the to_be_sent damage, and then submit a copy command in the block
 * handler.
 *
 * This means that when new damage happens, if there is already pending
 * damage, that must first be unioned onto to_be_sent, and then the new
 * damage must be stored in pending_copy.
 * 
 * The compat_qxl_screen_t struct contains two regions, "pending_copy" and 
 * "to_be_sent". 
 *
 * Pending copy is 
 * 
 */
static void
compat_qxl_on_damage (DamagePtr pDamage, RegionPtr pRegion, pointer closure)
{
    compat_qxl_screen_t *compat_qxl = closure;

/*     print_region ("damage", pRegion); */
    
/*     print_region ("on_damage ", pRegion); */

    accept_damage (compat_qxl);

/*     print_region ("accepting, compat_qxl->to_be_sent is now", &compat_qxl->to_be_sent); */

    REGION_COPY (compat_qxl->pScrn->pScreen, &(compat_qxl->pending_copy), pRegion);
}


static Bool
compat_qxl_create_screen_resources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;
    Bool ret;
    PixmapPtr pPixmap;

    pScreen->CreateScreenResources = compat_qxl->create_screen_resources;
    ret = pScreen->CreateScreenResources (pScreen);
    pScreen->CreateScreenResources = compat_qxl_create_screen_resources;

    if (!ret)
	return FALSE;

    compat_qxl->damage = DamageCreate (compat_qxl_on_damage, NULL,
			        DamageReportRawRegion,
				TRUE, pScreen, compat_qxl);


    pPixmap = pScreen->GetScreenPixmap(pScreen);

    if (!RegisterBlockAndWakeupHandlers(compat_qxl_block_handler, compat_qxl_wakeup_handler, compat_qxl))
	return FALSE;

    REGION_INIT (pScreen, &(compat_qxl->pending_copy), NullBox, 0);

    REGION_INIT (pScreen, &(compat_qxl->to_be_sent), NullBox, 0);
 
    DamageRegister (&pPixmap->drawable, compat_qxl->damage);
    return TRUE;
}

static PixmapPtr 
get_window_pixmap (DrawablePtr pDrawable, int *xoff, int *yoff)
{
    ScreenPtr pScreen = pDrawable->pScreen;
    PixmapPtr result;

    if (pDrawable->type != DRAWABLE_WINDOW)
	return NULL;

    result = pScreen->GetWindowPixmap ((WindowPtr)pDrawable);

    *xoff = pDrawable->x;
    *yoff = pDrawable->y;

    return result;
}

static void
compat_qxl_poly_fill_rect (DrawablePtr pDrawable,
		 GCPtr	     pGC,
		 int	     nrect,
		 xRectangle *prect)
{
    ScrnInfoPtr pScrn = xf86Screens[pDrawable->pScreen->myNum];
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap;
    int xoff, yoff;

    if ((pPixmap = get_window_pixmap (pDrawable, &xoff, &yoff))	&&
	pGC->fillStyle == FillSolid				&&
	pGC->alu == GXcopy					&&
	(unsigned int)pGC->planemask == FB_ALLONES)
    {
	RegionPtr pReg = RECTS_TO_REGION (pScreen, nrect, prect, CT_UNSORTED);
	RegionPtr pClip = fbGetCompositeClip (pGC);
	BoxPtr pBox;
	int nbox;
	
	REGION_TRANSLATE(pScreen, pReg, xoff, yoff);
	REGION_INTERSECT(pScreen, pReg, pClip, pReg);

	pBox = REGION_RECTS (pReg);
	nbox = REGION_NUM_RECTS (pReg);

	while (nbox--)
	{
	    struct compat_qxl_rect qrect;

	    qrect.left = pBox->x1;
	    qrect.right = pBox->x2;
	    qrect.top = pBox->y1;
	    qrect.bottom = pBox->y2;

	    submit_fill (compat_qxl, &qrect, pGC->fgPixel);

	    pBox++;
	}

	REGION_DESTROY (pScreen, pReg);
    }
    
    fbPolyFillRect (pDrawable, pGC, nrect, prect);
}

static void
compat_qxl_copy_n_to_n (DrawablePtr    pSrcDrawable,
		 DrawablePtr    pDstDrawable,
		 GCPtr	        pGC,
		 BoxPtr	        pbox,
		 int	        nbox,
		 int	        dx,
		 int	        dy,
		 Bool	        reverse,
		 Bool	        upsidedown,
		 Pixel	        bitplane,
		 void	       *closure)
{
    ScreenPtr pScreen = pSrcDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;
    int src_xoff, src_yoff;
    int dst_xoff, dst_yoff;
    PixmapPtr pSrcPixmap, pDstPixmap;

    if ((pSrcPixmap = get_window_pixmap (pSrcDrawable, &src_xoff, &src_yoff)) &&
	(pDstPixmap = get_window_pixmap (pDstDrawable, &dst_xoff, &dst_yoff)))
    {
	int n = nbox;
	BoxPtr b = pbox;
	
	assert (pSrcPixmap == pDstPixmap);

/* 	ErrorF ("Accelerated copy: %d boxes\n", n); */

	/* At this point we know that any pending damage must
	 * have been caused by whatever copy operation triggered us.
	 * 
	 * Therefore we can clear it.
	 *
	 * We couldn't clear it at the toplevel function because 
	 * the copy might end up being empty, in which case no
	 * damage would have been generated. Which means the
	 * pending damage would have been caused by some
	 * earlier operation.
	 */
	if (n)
	{
/* 	    ErrorF ("Clearing pending damage\n"); */
	    clear_pending_damage (compat_qxl);
	    
	    /* We have to do this because the copy will cause the damage
	     * to be sent to move.
	     * 
	     * Instead of just sending the bits, we could also move
	     * the existing damage around; however that's a bit more 
	     * complex, and the performance win is unlikely to be
	     * very big.
	     */
	    compat_qxl_send_copies (compat_qxl);
	}
    
	while (n--)
	{
	    struct compat_qxl_drawable *drawable;
	    struct compat_qxl_rect qrect;
	    
	    qrect.top = b->y1;
	    qrect.bottom = b->y2;
	    qrect.left = b->x1;
	    qrect.right = b->x2;

/* 	    ErrorF ("   Translate %d %d %d %d by %d %d (offsets %d %d)\n", */
/* 		    b->x1, b->y1, b->x2, b->y2, */
/* 		    dx, dy, dst_xoff, dst_yoff); */
	    
	    drawable = make_drawable (compat_qxl, QXL_COPY_BITS, &qrect);
	    drawable->u.copy_bits.src_pos.x = b->x1 + dx;
	    drawable->u.copy_bits.src_pos.y = b->y1 + dy;

	    push_drawable (compat_qxl, drawable);

#if 0
	    if (closure)
		compat_qxl_usleep (1000000);
#endif
	    
#if 0
	    submit_fill (compat_qxl, &qrect, rand());
#endif

	    b++;
	}
    }
/*     else */
/* 	ErrorF ("Unaccelerated copy\n"); */

    fbCopyNtoN (pSrcDrawable, pDstDrawable, pGC, pbox, nbox, dx, dy, reverse, upsidedown, bitplane, closure);
}

static RegionPtr
compat_qxl_copy_area(DrawablePtr pSrcDrawable, DrawablePtr pDstDrawable, GCPtr pGC,
	    int srcx, int srcy, int width, int height, int dstx, int dsty)
{
    if (pSrcDrawable->type == DRAWABLE_WINDOW &&
	pDstDrawable->type == DRAWABLE_WINDOW)
    {
	RegionPtr res;

/* 	ErrorF ("accelerated copy %d %d %d %d %d %d\n",  */
/* 		srcx, srcy, width, height, dstx, dsty); */

	res = fbDoCopy (pSrcDrawable, pDstDrawable, pGC,
			srcx, srcy, width, height, dstx, dsty,
			compat_qxl_copy_n_to_n, 0, NULL);

	return res;
    }
    else
    {
/* 	ErrorF ("Falling back %d %d %d %d %d %d\n",  */
/* 		srcx, srcy, width, height, dstx, dsty); */

	return fbCopyArea (pSrcDrawable, pDstDrawable, pGC,
			   srcx, srcy, width, height, dstx, dsty);
    }
}

static void
compat_qxl_fill_region_solid (DrawablePtr pDrawable, RegionPtr pRegion, Pixel pixel)
{
    ScreenPtr pScreen = pDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap;
    int xoff, yoff;

    if ((pPixmap = get_window_pixmap (pDrawable, &xoff, &yoff)))
    {
	int nbox = REGION_NUM_RECTS (pRegion);
	BoxPtr pBox = REGION_RECTS (pRegion);

	while (nbox--)
	{
	    struct compat_qxl_rect qrect;

	    qrect.left = pBox->x1;
	    qrect.right = pBox->x2;
	    qrect.top = pBox->y1;
	    qrect.bottom = pBox->y2;

	    submit_fill (compat_qxl, &qrect, pixel);

	    pBox++;
	}
    }

    fbFillRegionSolid (pDrawable, pRegion, 0,
		       fbReplicatePixel (pixel, pDrawable->bitsPerPixel));
}

static void
compat_qxl_copy_window (WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
{
    RegionRec rgnDst;
    int dx, dy;

    dx = ptOldOrg.x - pWin->drawable.x;
    dy = ptOldOrg.y - pWin->drawable.y;

    REGION_TRANSLATE (pScreen, prgnSrc, -dx, -dy);

    REGION_INIT (pScreen, &rgnDst, NullBox, 0);

    REGION_INTERSECT(pScreen, &rgnDst, &pWin->borderClip, prgnSrc);

    fbCopyRegion (&pWin->drawable, &pWin->drawable,
		  NULL, 
		  &rgnDst, dx, dy, compat_qxl_copy_n_to_n, 0, NULL);

    REGION_UNINIT (pScreen, &rgnDst);

/*     REGION_TRANSLATE (pScreen, prgnSrc, dx, dy); */
    
/*     fbCopyWindow (pWin, ptOldOrg, prgnSrc); */
}

static int
compat_qxl_create_gc (GCPtr pGC)
{
    static GCOps ops;
    static int initialized;
    
    if (!fbCreateGC (pGC))
	return FALSE;

    if (!initialized)
    {
	ops = *pGC->ops;
	ops.PolyFillRect = compat_qxl_poly_fill_rect;
	ops.CopyArea = compat_qxl_copy_area;

	initialized = TRUE;
    }
    
    pGC->ops = &ops;
    return TRUE;
}

static Bool
compat_qxl_screen_init(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;
    struct compat_qxl_rom *rom;
    struct compat_qxl_ram_header *ram_header;
    VisualPtr visual;

    CHECK_POINT();

    compat_qxl->pScrn = pScrn;
    
    if (!compat_qxl_map_memory(compat_qxl, scrnIndex))
	return FALSE;

    rom = compat_qxl->rom;
    ram_header = (void *)((unsigned long)compat_qxl->ram + (unsigned long)compat_qxl->rom->ram_header_offset);

    compat_qxl_save_state(pScrn);
    compat_qxl_blank_screen(pScreen, SCREEN_SAVER_ON);
    
    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
			  pScrn->rgbBits, pScrn->defaultVisual))
	goto out;
    if (!miSetPixmapDepths())
	goto out;

    /* Note we do this before setting pScrn->virtualY to match our current
       mode, so as to allocate a buffer large enough for the largest mode.
       FIXME: add support for resizing the framebuffer on modeset. */
    compat_qxl->fb = xcalloc(pScrn->virtualY * pScrn->displayWidth, 4);
    if (!compat_qxl->fb)
	goto out;

    pScrn->virtualX = pScrn->currentMode->HDisplay;
    pScrn->virtualY = pScrn->currentMode->VDisplay;
    
    if (!fbScreenInit(pScreen, compat_qxl->fb,
		      pScrn->currentMode->HDisplay,
		      pScrn->currentMode->VDisplay,
		      pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
		      pScrn->bitsPerPixel))
    {
	goto out;
    }

    visual = pScreen->visuals + pScreen->numVisuals;
    while (--visual >= pScreen->visuals) 
    {
	if ((visual->class | DynamicClass) == DirectColor) 
	{
	    visual->offsetRed = pScrn->offset.red;
	    visual->offsetGreen = pScrn->offset.green;
	    visual->offsetBlue = pScrn->offset.blue;
	    visual->redMask = pScrn->mask.red;
	    visual->greenMask = pScrn->mask.green;
	    visual->blueMask = pScrn->mask.blue;
	}
    }

    
    fbPictureInit(pScreen, 0, 0);

    compat_qxl->create_screen_resources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = compat_qxl_create_screen_resources;

    /* Set up resources */
    compat_qxl->mem = compat_qxl_mem_create ((void *)((unsigned long)compat_qxl->ram + (unsigned long)rom->pages_offset),
			       rom->num_io_pages * getpagesize());
    compat_qxl->io_pages = (void *)((unsigned long)compat_qxl->ram + (unsigned long)rom->pages_offset);
    compat_qxl->io_pages_physical = (void *)((unsigned long)compat_qxl->ram_physical + (unsigned long)rom->pages_offset);

    compat_qxl->command_ring = compat_qxl_ring_create (&(ram_header->cmd_ring_hdr),
					 sizeof (struct compat_qxl_command),
					 32, compat_qxl->io_base + QXL_IO_NOTIFY_CMD);
    compat_qxl->cursor_ring = compat_qxl_ring_create (&(ram_header->cursor_ring_hdr),
					sizeof (struct compat_qxl_command),
					32, compat_qxl->io_base + QXL_IO_NOTIFY_CURSOR);
    compat_qxl->release_ring = compat_qxl_ring_create (&(ram_header->release_ring_hdr),
					 sizeof (uint64_t),
					 8, 0);
					 
    /* xf86DPMSInit(pScreen, xf86DPMSSet, 0); */

#if 0 /* XV accel */
    compat_qxlInitVideo(pScreen);
#endif

    pScreen->SaveScreen = compat_qxl_blank_screen;
    compat_qxl->close_screen = pScreen->CloseScreen;
    pScreen->CloseScreen = compat_qxl_close_screen;

    compat_qxl->create_gc = pScreen->CreateGC;
    pScreen->CreateGC = compat_qxl_create_gc;

#if 0
    compat_qxl->paint_window_background = pScreen->PaintWindowBackground;
    compat_qxl->paint_window_border = pScreen->PaintWindowBorder;
#endif
    compat_qxl->copy_window = pScreen->CopyWindow;
#if 0
    pScreen->PaintWindowBackground = compat_qxl_paint_window;
    pScreen->PaintWindowBorder = compat_qxl_paint_window;
#endif
    pScreen->CopyWindow = compat_qxl_copy_window;

    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    if (!miCreateDefColormap(pScreen))
	goto out;

    compat_qxl_cursor_init (pScreen);
    
    CHECK_POINT();

    compat_qxl_switch_mode(scrnIndex, pScrn->currentMode, 0);

    CHECK_POINT();
    
    return TRUE;

out:
    return FALSE;
}

static Bool
compat_qxl_enter_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

    compat_qxl_save_state(pScrn);
    compat_qxl_switch_mode(scrnIndex, pScrn->currentMode, 0);

    return TRUE;
}

static void
compat_qxl_leave_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

    compat_qxl_restore_state(pScrn);
}

static Bool
compat_qxl_color_setup(ScrnInfoPtr pScrn)
{
    int scrnIndex = pScrn->scrnIndex;
    Gamma gzeros = { 0.0, 0.0, 0.0 };
    rgb rzeros = { 0, 0, 0 };

    if (!xf86SetDepthBpp(pScrn, 0, 0, 0, Support32bppFb))
	return FALSE;

    if (pScrn->depth != 15 && pScrn->depth != 24) 
    {
	xf86DrvMsg(scrnIndex, X_ERROR, "Depth %d is not supported\n",
		   pScrn->depth);
	return FALSE;
    }
    xf86PrintDepthBpp(pScrn);

    if (!xf86SetWeight(pScrn, rzeros, rzeros))
	return FALSE;

    if (!xf86SetDefaultVisual(pScrn, -1))
	return FALSE;

    if (!xf86SetGamma(pScrn, gzeros))
	return FALSE;

    return TRUE;
}

static void
print_modes (compat_qxl_screen_t *compat_qxl, int scrnIndex)
{
    int i;

    for (i = 0; i < compat_qxl->num_modes; ++i)
    {
	struct compat_qxl_mode *m = compat_qxl->modes + i;

	xf86DrvMsg (scrnIndex, X_INFO,
		    "%d: %dx%d, %d bits, stride %d, %dmm x %dmm, orientation %d\n",
		    m->id, m->x_res, m->y_res, m->bits, m->stride, m->x_mili,
		    m->y_mili, m->orientation);
    }
}

static Bool
compat_qxl_check_device(ScrnInfoPtr pScrn, compat_qxl_screen_t *compat_qxl)
{
    int scrnIndex = pScrn->scrnIndex;
    struct compat_qxl_rom *rom = compat_qxl->rom;
    struct compat_qxl_ram_header *ram_header = (void *)((unsigned long)compat_qxl->ram + rom->ram_header_offset);

    CHECK_POINT();
    
    if (rom->magic != 0x4f525851) { /* "QXRO" little-endian */
	xf86DrvMsg(scrnIndex, X_ERROR, "Bad ROM signature %x\n", rom->magic);
	return FALSE;
    }

    xf86DrvMsg(scrnIndex, X_INFO, "Device version %d.%d\n",
	       rom->id, rom->update_id);

    xf86DrvMsg(scrnIndex, X_INFO, "Compression level %d, log level %d\n",
	       rom->compression_level,
	       rom->log_level);

    xf86DrvMsg(scrnIndex, X_INFO, "Currently using mode #%d, list at 0x%x\n",
	       rom->mode, rom->modes_offset);

    xf86DrvMsg(scrnIndex, X_INFO, "%d io pages at 0x%x\n",
	       rom->num_io_pages, rom->pages_offset);

    xf86DrvMsg(scrnIndex, X_INFO, "%d byte draw area at 0x%x\n",
	       rom->draw_area_size, rom->draw_area_offset);

    xf86DrvMsg(scrnIndex, X_INFO, "RAM header offset: 0x%x\n", rom->ram_header_offset);

    if (ram_header->magic != 0x41525851) { /* "QXRA" little-endian */
	xf86DrvMsg(scrnIndex, X_ERROR, "Bad RAM signature %x at %p\n",
		   ram_header->magic,
		   &ram_header->magic);
	return FALSE;
    }

    xf86DrvMsg(scrnIndex, X_INFO, "Correct RAM signature %x\n", 
	       ram_header->magic);

    compat_qxl->draw_area_offset = rom->draw_area_offset;
    compat_qxl->draw_area_size = rom->draw_area_size;
    pScrn->videoRam = rom->draw_area_size / 1024;
    
    return TRUE;
}

static int
compat_qxl_find_native_mode(ScrnInfoPtr pScrn, DisplayModePtr p)
{
    int i;
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;

    CHECK_POINT();
    
    for (i = 0; i < compat_qxl->num_modes; i++) 
    {
	struct compat_qxl_mode *m = compat_qxl->modes + i;

	if (m->x_res == p->HDisplay &&
	    m->y_res == p->VDisplay &&
	    m->bits == pScrn->bitsPerPixel)
	{
	    if (m->bits == 16) 
	    {
		/* What QXL calls 16 bit is actually x1r5g5b515 */
		if (pScrn->depth == 15)
		    return i;
	    }
	    else if (m->bits == 32)
	    {
		/* What QXL calls 32 bit is actually x8r8g8b8 */
		if (pScrn->depth == 24)
		    return i;
	    }
	}
    }

    return -1;
}

static ModeStatus
compat_qxl_valid_mode(int scrn, DisplayModePtr p, Bool flag, int pass)
{
    ScrnInfoPtr pScrn = xf86Screens[scrn];
    compat_qxl_screen_t *compat_qxl = pScrn->driverPrivate;
    int bpp = pScrn->bitsPerPixel;
    int mode_idx;

    /* FIXME: I don't think this is necessary now that we report the
     * correct amount of video ram?
     */
    if (p->HDisplay * p->VDisplay * (bpp/8) > compat_qxl->draw_area_size)
	return MODE_MEM;

    mode_idx = compat_qxl_find_native_mode (pScrn, p);
    if (mode_idx == -1)
	return MODE_NOMODE;

    p->Private = (void *)(unsigned long)mode_idx;
    
    return MODE_OK;
}

static void compat_qxl_add_mode(ScrnInfoPtr pScrn, int width, int height, int type)
{
    DisplayModePtr mode;

    /* Skip already present modes */
    for (mode = pScrn->monitor->Modes; mode; mode = mode->next)
        if (mode->HDisplay == width && mode->VDisplay == height)
            return;

    mode = xnfcalloc(1, sizeof(DisplayModeRec));

    mode->status = MODE_OK;
    mode->type = type;
    mode->HDisplay   = width;
    mode->HSyncStart = (width * 105 / 100 + 7) & ~7;
    mode->HSyncEnd   = (width * 115 / 100 + 7) & ~7;
    mode->HTotal     = (width * 130 / 100 + 7) & ~7;
    mode->VDisplay   = height;
    mode->VSyncStart = height + 1;
    mode->VSyncEnd   = height + 4;
    mode->VTotal     = height * 1035 / 1000;
    mode->Clock = mode->HTotal * mode->VTotal * 60 / 1000;
    mode->Flags = V_NHSYNC | V_PVSYNC;

    xf86SetModeDefaultName(mode);
    xf86ModesAdd(pScrn->monitor->Modes, mode);
}

static Bool
compat_qxl_pre_init(ScrnInfoPtr pScrn, int flags)
{
    int i, scrnIndex = pScrn->scrnIndex;
    compat_qxl_screen_t *compat_qxl = NULL;
    ClockRangePtr clockRanges = NULL;
    int *linePitches = NULL;
    DisplayModePtr mode;
    unsigned int max_x = 0, max_y = 0;

    CHECK_POINT();
    
    /* zaphod mode is for suckers and i choose not to implement it */
    if (xf86IsEntityShared(pScrn->entityList[0])) {
	xf86DrvMsg(scrnIndex, X_ERROR, "No Zaphod mode for you\n");
	return FALSE;
    }

    if (!pScrn->driverPrivate)
	pScrn->driverPrivate = xnfcalloc(sizeof(compat_qxl_screen_t), 1);
    compat_qxl = pScrn->driverPrivate;
    
    compat_qxl->entity = xf86GetEntityInfo(pScrn->entityList[0]);
    compat_qxl->pci = xf86GetPciInfoForEntity(compat_qxl->entity->index);
#ifndef XSERVER_LIBPCIACCESS
    compat_qxl->pci_tag = pciTag(compat_qxl->pci->bus, compat_qxl->pci->device, compat_qxl->pci->func);
#endif

    pScrn->monitor = pScrn->confScreen->monitor;

    if (!compat_qxl_color_setup(pScrn))
	goto out;

    /* option parsing and card differentiation */
    xf86CollectOptions(pScrn, NULL);
    
    if (!compat_qxl_map_memory(compat_qxl, scrnIndex))
	goto out;

    if (!compat_qxl_check_device(pScrn, compat_qxl))
	goto out;

    /* ddc stuff here */

    clockRanges = xnfcalloc(sizeof(ClockRange), 1);
    clockRanges->next = NULL;
    clockRanges->minClock = 10000;
    clockRanges->maxClock = 400000;
    clockRanges->clockIndex = -1;
    clockRanges->interlaceAllowed = clockRanges->doubleScanAllowed = 0;
    clockRanges->ClockMulFactor = clockRanges->ClockDivFactor = 1;
    pScrn->progClock = TRUE;

    /* override QXL monitor stuff */
    if (pScrn->monitor->nHsync <= 0) {
	pScrn->monitor->hsync[0].lo =  29.0;
	pScrn->monitor->hsync[0].hi = 160.0;
	pScrn->monitor->nHsync = 1;
    }
    if (pScrn->monitor->nVrefresh <= 0) {
	pScrn->monitor->vrefresh[0].lo = 50;
	pScrn->monitor->vrefresh[0].hi = 75;
	pScrn->monitor->nVrefresh = 1;
    }

    /* Add any modes not in xorg's default mode list */
    for (i = 0; i < compat_qxl->num_modes; i++)
        if (compat_qxl->modes[i].orientation == 0) {
            compat_qxl_add_mode(pScrn, compat_qxl->modes[i].x_res, compat_qxl->modes[i].y_res,
                         M_T_DRIVER);
            if (compat_qxl->modes[i].x_res > max_x)
                max_x = compat_qxl->modes[i].x_res;
            if (compat_qxl->modes[i].y_res > max_y)
                max_y = compat_qxl->modes[i].y_res;
        }

    if (pScrn->display->virtualX == 0 && pScrn->display->virtualY == 0) {
        /* It is possible for the largest x + largest y size combined leading
           to a virtual size which will not fit into the framebuffer when this
           happens we prefer max width and make height as large as possible */
        if (max_x * max_y * (pScrn->bitsPerPixel / 8) > compat_qxl->draw_area_size)
            pScrn->display->virtualY = compat_qxl->draw_area_size /
                                       (max_x * (pScrn->bitsPerPixel / 8));
        else
            pScrn->display->virtualY = max_y;

    	pScrn->display->virtualX = max_x;
    }

    if (0 >= xf86ValidateModes(pScrn, pScrn->monitor->Modes,
			       pScrn->display->modes, clockRanges, linePitches,
			       128, max_x, 128 * 4, 128, max_y,
			       pScrn->display->virtualX,
			       pScrn->display->virtualY,
			       128 * 1024 * 1024, LOOKUP_BEST_REFRESH))
	goto out;

    CHECK_POINT();
    
    xf86PruneDriverModes(pScrn);
    pScrn->currentMode = pScrn->modes;
    /* If no modes are specified in xorg.conf, default to 1024x768 */
    if (pScrn->display->modes == NULL || pScrn->display->modes[0] == NULL)
        for (mode = pScrn->modes; mode; mode = mode->next)
            if (mode->HDisplay == 1024 && mode->VDisplay == 768) {
                pScrn->currentMode = mode;
                break;
            }

    xf86PrintModes(pScrn);
    xf86SetDpi(pScrn, 0, 0);

    if (!xf86LoadSubModule(pScrn, "fb") ||
	!xf86LoadSubModule(pScrn, "ramdac") ||
	!xf86LoadSubModule(pScrn, "vgahw"))
    {
	goto out;
    }

    print_modes (compat_qxl, scrnIndex);

    /* VGA hardware initialisation */
    if (!vgaHWGetHWRec(pScrn))
        return FALSE;

    /* hate */
    compat_qxl_unmap_memory(compat_qxl, scrnIndex);

    CHECK_POINT();
    
    xf86DrvMsg(scrnIndex, X_INFO, "PreInit complete\n");
    return TRUE;

out:
    if (clockRanges)
	xfree(clockRanges);
    if (compat_qxl)
	xfree(compat_qxl);

    return FALSE;
}

#ifdef XSERVER_LIBPCIACCESS
enum compat_qxl_class
{
    CHIP_QXL_1,
};

static const struct pci_id_match compat_qxl_device_match[] = {
    {
	PCI_VENDOR_RED_HAT, PCI_CHIP_QXL_0100, PCI_MATCH_ANY, PCI_MATCH_ANY,
	0x00030000, 0x00ffffff, CHIP_QXL_1
    },

    { 0 },
};
#endif

static SymTabRec compat_qxlChips[] =
{
    { PCI_CHIP_QXL_0100,	"QXL 1", },
    { -1, NULL }
};

#ifndef XSERVER_LIBPCIACCESS
static PciChipsets compat_qxlPciChips[] =
{
    { PCI_CHIP_QXL_0100,    PCI_CHIP_QXL_0100,	RES_SHARED_VGA },
    { -1, -1, RES_UNDEFINED }
};
#endif

static void
compat_qxl_identify(int flags)
{
    xf86PrintChipsets("compat_qxl", "Driver for QXL virtual graphics", compat_qxlChips);
}

void
compat_init_scrn(ScrnInfoPtr pScrn)
{
    pScrn->driverVersion    = 0;
    pScrn->driverName	    = pScrn->name = "compat_qxl";
    pScrn->PreInit	    = compat_qxl_pre_init;
    pScrn->ScreenInit	    = compat_qxl_screen_init;
    pScrn->SwitchMode	    = compat_qxl_switch_mode;
    pScrn->ValidMode	    = compat_qxl_valid_mode;
    pScrn->EnterVT	    = compat_qxl_enter_vt;
    pScrn->LeaveVT	    = compat_qxl_leave_vt;
}
