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

/** \file qxl_driver.c
 * \author Adam Jackson <ajax@redhat.com>
 *
 * This is qxl, a driver for the Qumranet paravirtualized graphics device
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
garbage_collect (qxl_screen_t *qxl)
{
    uint64_t id;
    int i = 0;
    
    while (qxl_ring_pop (qxl->release_ring, &id))
    {
	while (id)
	{
	    /* We assume that there the two low bits of a pointer are
	     * available. If the low one is set, then the command in
	     * question is a cursor command
	     */
#define POINTER_MASK ((1 << 2) - 1)
	    
	    union qxl_release_info *info = u64_to_pointer (id & ~POINTER_MASK);
	    struct qxl_cursor_cmd *cmd = (struct qxl_cursor_cmd *)info;
	    struct qxl_drawable *drawable = (struct qxl_drawable *)info;
	    int is_cursor = FALSE;

	    if ((id & POINTER_MASK) == 1)
		is_cursor = TRUE;

	    if (is_cursor && cmd->type == QXL_CURSOR_SET)
	    {
		struct qxl_cursor *cursor = (void *)virtual_address (
		    qxl, u64_to_pointer (cmd->u.set.shape));

		qxl_free (qxl->mem, cursor);
	    }
	    else if (!is_cursor && drawable->type == QXL_DRAW_COPY)
	    {
		struct qxl_image *image = virtual_address (
		    qxl, u64_to_pointer (drawable->u.copy.src_bitmap));

		qxl_image_destroy (qxl, image);
	    }
	    
	    id = info->next;
	    
	    qxl_free (qxl->mem, info);
	}
    }

    return i > 0;
}

static void
qxl_usleep (int useconds)
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
push_update_area (qxl_screen_t *qxl, const struct qxl_rect *area)
{
    struct qxl_update_cmd *update = qxl_allocnf (qxl, sizeof *update);
    struct qxl_command cmd;

    update->release_info.id = (uint64_t)update;
    update->area = *area;
    update->update_id = 0;

    cmd.type = QXL_CMD_UDPATE;
    cmd.data = physical_address (qxl, update);

    qxl_ring_push (qxl->command_ring, &cmd);
}
#endif

void *
qxl_allocnf (qxl_screen_t *qxl, unsigned long size)
{
    void *result;
    int n_attempts = 0;
    static int nth_oom = 1;

    garbage_collect (qxl);
    
    while (!(result = qxl_alloc (qxl->mem, size)))
    {
	struct qxl_ram_header *ram_header = (void *)((unsigned long)qxl->ram +
						     qxl->rom->ram_header_offset);
	
	/* Rather than go out of memory, we simply tell the
	 * device to dump everything
	 */
	ram_header->update_area.top = 0;
	ram_header->update_area.bottom = 1280;
	ram_header->update_area.left = 0;
	ram_header->update_area.right = 800;
	
	outb (qxl->io_base + QXL_IO_UPDATE_AREA, 0);
	
 	ErrorF ("eliminated memory (%d)\n", nth_oom++);

	outb (qxl->io_base + QXL_IO_NOTIFY_OOM, 0);

	qxl_usleep (10000);
	
	if (garbage_collect (qxl))
	{
	    n_attempts = 0;
	}
	else if (++n_attempts == 1000)
	{
	    qxl_mem_dump_stats (qxl->mem, "Out of mem - stats\n");
	    
	    fprintf (stderr, "Out of memory\n");
	    exit (1);
	}
    }

    return result;
}

static Bool
qxl_blank_screen(ScreenPtr pScreen, int mode)
{
    return TRUE;
}

static void
qxl_unmap_memory(qxl_screen_t *qxl, int scrnIndex)
{
#ifdef XSERVER_LIBPCIACCESS
    if (qxl->ram)
	pci_device_unmap_range(qxl->pci, qxl->ram, qxl->pci->regions[0].size);
    if (qxl->vram)
	pci_device_unmap_range(qxl->pci, qxl->vram, qxl->pci->regions[1].size);
    if (qxl->rom)
	pci_device_unmap_range(qxl->pci, qxl->rom, qxl->pci->regions[2].size);
#else
    if (qxl->ram)
	xf86UnMapVidMem(scrnIndex, qxl->ram, (1 << qxl->pci->size[0]));
    if (qxl->vram)
	xf86UnMapVidMem(scrnIndex, qxl->vram, (1 << qxl->pci->size[1]));
    if (qxl->rom)
	xf86UnMapVidMem(scrnIndex, qxl->rom, (1 << qxl->pci->size[2]));
#endif

    qxl->ram = qxl->ram_physical = qxl->vram = qxl->rom = NULL;

    qxl->num_modes = 0;
    qxl->modes = NULL;
}

static Bool
qxl_map_memory(qxl_screen_t *qxl, int scrnIndex)
{
#ifdef XSERVER_LIBPCIACCESS
    pci_device_map_range(qxl->pci, qxl->pci->regions[0].base_addr, 
			 qxl->pci->regions[0].size,
			 PCI_DEV_MAP_FLAG_WRITABLE | PCI_DEV_MAP_FLAG_WRITE_COMBINE,
			 &qxl->ram);
    qxl->ram_physical = u64_to_pointer (qxl->pci->regions[0].base_addr);

    pci_device_map_range(qxl->pci, qxl->pci->regions[1].base_addr, 
			 qxl->pci->regions[1].size,
			 PCI_DEV_MAP_FLAG_WRITABLE,
			 &qxl->vram);

    pci_device_map_range(qxl->pci, qxl->pci->regions[2].base_addr, 
			 qxl->pci->regions[2].size, 0,
			 (void **)&qxl->rom);

    qxl->io_base = qxl->pci->regions[3].base_addr;
#else
    qxl->ram = xf86MapPciMem(scrnIndex, VIDMEM_FRAMEBUFFER,
			     qxl->pci_tag, qxl->pci->memBase[0],
			     (1 << qxl->pci->size[0]));
    qxl->ram_physical = (void *)qxl->pci->memBase[0];
    
    qxl->vram = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			      qxl->pci_tag, qxl->pci->memBase[1],
			      (1 << qxl->pci->size[1]));
    
    qxl->rom = xf86MapPciMem(scrnIndex, VIDMEM_MMIO | VIDMEM_MMIO_32BIT,
			     qxl->pci_tag, qxl->pci->memBase[2],
			     (1 << qxl->pci->size[2]));
    
    qxl->io_base = qxl->pci->ioBase[3];
#endif
    if (!qxl->ram || !qxl->vram || !qxl->rom)
	return FALSE;

    xf86DrvMsg(scrnIndex, X_INFO, "ram at %p; vram at %p; rom at %p\n",
	       qxl->ram, qxl->vram, qxl->rom);

    qxl->num_modes = *(uint32_t *)((uint8_t *)qxl->rom + qxl->rom->modes_offset);
    qxl->modes = (struct qxl_mode *)(((uint8_t *)qxl->rom) + qxl->rom->modes_offset + 4);

    return TRUE;
}

static void
qxl_save_state(ScrnInfoPtr pScrn)
{
    qxl_screen_t *qxl = pScrn->driverPrivate;

    vgaHWSaveFonts(pScrn, &qxl->vgaRegs);
}

static void
qxl_restore_state(ScrnInfoPtr pScrn)
{
    qxl_screen_t *qxl = pScrn->driverPrivate;

    vgaHWRestoreFonts(pScrn, &qxl->vgaRegs);
}

static Bool
qxl_close_screen(int scrnIndex, ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxl_screen_t *qxl = pScrn->driverPrivate;

    if (pScrn->vtSema) {
        qxl_restore_state(pScrn);
	qxl_unmap_memory(qxl, scrnIndex);
    }
    pScrn->vtSema = FALSE;

    xfree(qxl->fb);

    pScreen->CreateScreenResources = qxl->create_screen_resources;
    pScreen->CloseScreen = qxl->close_screen;

    return pScreen->CloseScreen(scrnIndex, pScreen);
}

static Bool
qxl_switch_mode(int scrnIndex, DisplayModePtr p, int flags)
{
    qxl_screen_t *qxl = xf86Screens[scrnIndex]->driverPrivate;
    int mode_index = (int)(unsigned long)p->Private;
    struct qxl_mode *m = qxl->modes + mode_index;
    ScreenPtr pScreen = qxl->pScrn->pScreen;

    if (!m)
	return FALSE;

    /* if (debug) */
    xf86DrvMsg (scrnIndex, X_INFO, "Setting mode %d (%d x %d) (%d x %d) %p\n",
		m->id, m->x_res, m->y_res, p->HDisplay, p->VDisplay, p);

    outb(qxl->io_base + QXL_IO_RESET, 0);
    
    outb(qxl->io_base + QXL_IO_SET_MODE, m->id);

    qxl->bytes_per_pixel = (qxl->pScrn->bitsPerPixel + 7) / 8;

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
		qxl->pScrn->displayWidth * qxl->bytes_per_pixel,
		NULL);
	}
    }
    
    if (qxl->mem)
    {
	qxl_mem_free_all (qxl->mem);
	qxl_drop_image_cache (qxl);
    }

    
    return TRUE;
}

static void
push_drawable (qxl_screen_t *qxl, struct qxl_drawable *drawable)
{
    struct qxl_command cmd;

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
    if (qxl->rom->mode != ~0)
    {
	cmd.type = QXL_CMD_DRAW;
	cmd.data = physical_address (qxl, drawable);
	    
	qxl_ring_push (qxl->command_ring, &cmd);
    }
}

static struct qxl_drawable *
make_drawable (qxl_screen_t *qxl, uint8_t type,
	       const struct qxl_rect *rect
	       /* , pRegion clip */)
{
    struct qxl_drawable *drawable;

    CHECK_POINT();
    
    drawable = qxl_allocnf (qxl, sizeof *drawable);

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

    drawable->mm_time = qxl->rom->mm_clock;

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
undamage_box (qxl_screen_t *qxl, const struct qxl_rect *rect)
{
    RegionRec region;
    BoxRec box;

    box.x1 = rect->left;
    box.y1 = rect->top;
    box.x2 = rect->right;
    box.y2 = rect->bottom;

    REGION_INIT (qxl->pScrn->pScreen, &region, &box, 0);

    REGION_SUBTRACT (qxl->pScrn->pScreen, &(qxl->pending_copy), &(qxl->pending_copy), &region);

    REGION_EMPTY (qxl->pScrn->pScreen, &(qxl->pending_copy));
}

static void
clear_pending_damage (qxl_screen_t *qxl)
{
    REGION_EMPTY (qxl->pScrn->pScreen, &(qxl->pending_copy));
}

static void
submit_fill (qxl_screen_t *qxl, const struct qxl_rect *rect, uint32_t color)
{
    struct qxl_drawable *drawable;

    CHECK_POINT();
    
    drawable = make_drawable (qxl, QXL_DRAW_FILL, rect);

    CHECK_POINT();

    drawable->u.fill.brush.type = QXL_BRUSH_TYPE_SOLID;
    drawable->u.fill.brush.u.color = color;
    drawable->u.fill.rop_descriptor = ROPD_OP_PUT;
    drawable->u.fill.mask.flags = 0;
    drawable->u.fill.mask.pos.x = 0;
    drawable->u.fill.mask.pos.y = 0;
    drawable->u.fill.mask.bitmap = 0;

    push_drawable (qxl, drawable);

    undamage_box (qxl, rect);
}

static void
translate_rect (struct qxl_rect *rect)
{
    rect->right -= rect->left;
    rect->bottom -= rect->top;
    rect->left = rect->top = 0;
}

static void
submit_copy (qxl_screen_t *qxl, const struct qxl_rect *rect)
{
    struct qxl_drawable *drawable;
    ScrnInfoPtr pScrn = qxl->pScrn;

    if (rect->left == rect->right ||
	rect->top == rect->bottom)
    {
	/* Empty rectangle */
	return ;
    }
    
    drawable = make_drawable (qxl, QXL_DRAW_COPY, rect);

    drawable->u.copy.src_bitmap = physical_address (
	qxl, qxl_image_create (qxl, qxl->fb, rect->left, rect->top,
			       rect->right - rect->left,
			       rect->bottom - rect->top,
			       pScrn->displayWidth * qxl->bytes_per_pixel));
    drawable->u.copy.src_area = *rect;
    translate_rect (&drawable->u.copy.src_area);
    drawable->u.copy.rop_descriptor = ROPD_OP_PUT;
    drawable->u.copy.scale_mode = 0;
    drawable->u.copy.mask.flags = 0;
    drawable->u.copy.mask.pos.x = 0;
    drawable->u.copy.mask.pos.y = 0;
    drawable->u.copy.mask.bitmap = 0;

    push_drawable (qxl, drawable);
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
accept_damage (qxl_screen_t *qxl)
{
    REGION_UNION (qxl->pScrn->pScreen, &(qxl->to_be_sent), &(qxl->to_be_sent), 
		  &(qxl->pending_copy));

    REGION_EMPTY (qxl->pScrn->pScreen, &(qxl->pending_copy));
}

static void
qxl_send_copies (qxl_screen_t *qxl)
{
    BoxPtr pBox;
    int nbox;

    nbox = REGION_NUM_RECTS (&qxl->to_be_sent);
    pBox = REGION_RECTS (&qxl->to_be_sent);

/*      if (REGION_NUM_RECTS (&qxl->to_be_sent) > 0)  */
/*        	print_region ("send bits", &qxl->to_be_sent); */
    
    while (nbox--)
    {
	struct qxl_rect qrect;

	qrect.top = pBox->y1;
	qrect.left = pBox->x1;
	qrect.bottom = pBox->y2;
	qrect.right = pBox->x2;
	
	submit_copy (qxl, &qrect);

	pBox++;
    }

    REGION_EMPTY(qxl->pScrn->pScreen, &qxl->to_be_sent);
}

static void
paint_shadow (qxl_screen_t *qxl)
{
    struct qxl_rect qrect;

    qrect.top = 0;
    qrect.bottom = 1200;
    qrect.left = 0;
    qrect.right = 1600;

    submit_copy (qxl, &qrect);
}

static void
qxl_sanity_check (qxl_screen_t *qxl)
{
    /* read the mode back from the rom */
    if (!qxl->rom || !qxl->pScrn)
	return;

    if (qxl->rom->mode == ~0) 
    {
 	ErrorF("QXL device jumped back to VGA mode - resetting mode\n");
 	qxl_switch_mode(qxl->pScrn->scrnIndex, qxl->pScrn->currentMode, 0);
    }
}

static void
qxl_block_handler (pointer data, OSTimePtr pTimeout, pointer pRead)
{
    qxl_screen_t *qxl = (qxl_screen_t *) data;

    if (!qxl->pScrn->vtSema)
        return;

    qxl_sanity_check(qxl);

    accept_damage (qxl);

    qxl_send_copies (qxl);
}

static void
qxl_wakeup_handler (pointer data, int i, pointer LastSelectMask)
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
 * The qxl_screen_t struct contains two regions, "pending_copy" and 
 * "to_be_sent". 
 *
 * Pending copy is 
 * 
 */
static void
qxl_on_damage (DamagePtr pDamage, RegionPtr pRegion, pointer closure)
{
    qxl_screen_t *qxl = closure;

/*     print_region ("damage", pRegion); */
    
/*     print_region ("on_damage ", pRegion); */

    accept_damage (qxl);

/*     print_region ("accepting, qxl->to_be_sent is now", &qxl->to_be_sent); */

    REGION_COPY (qxl->pScrn->pScreen, &(qxl->pending_copy), pRegion);
}


static Bool
qxl_create_screen_resources(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    Bool ret;
    PixmapPtr pPixmap;

    pScreen->CreateScreenResources = qxl->create_screen_resources;
    ret = pScreen->CreateScreenResources (pScreen);
    pScreen->CreateScreenResources = qxl_create_screen_resources;

    if (!ret)
	return FALSE;

    qxl->damage = DamageCreate (qxl_on_damage, NULL,
			        DamageReportRawRegion,
				TRUE, pScreen, qxl);


    pPixmap = pScreen->GetScreenPixmap(pScreen);

    if (!RegisterBlockAndWakeupHandlers(qxl_block_handler, qxl_wakeup_handler, qxl))
	return FALSE;

    REGION_INIT (pScreen, &(qxl->pending_copy), NullBox, 0);

    REGION_INIT (pScreen, &(qxl->to_be_sent), NullBox, 0);
 
    DamageRegister (&pPixmap->drawable, qxl->damage);
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
qxl_poly_fill_rect (DrawablePtr pDrawable,
		 GCPtr	     pGC,
		 int	     nrect,
		 xRectangle *prect)
{
    ScrnInfoPtr pScrn = xf86Screens[pDrawable->pScreen->myNum];
    qxl_screen_t *qxl = pScrn->driverPrivate;
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
	    struct qxl_rect qrect;

	    qrect.left = pBox->x1;
	    qrect.right = pBox->x2;
	    qrect.top = pBox->y1;
	    qrect.bottom = pBox->y2;

	    submit_fill (qxl, &qrect, pGC->fgPixel);

	    pBox++;
	}

	REGION_DESTROY (pScreen, pReg);
    }
    
    fbPolyFillRect (pDrawable, pGC, nrect, prect);
}

static void
qxl_copy_n_to_n (DrawablePtr    pSrcDrawable,
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
    qxl_screen_t *qxl = pScrn->driverPrivate;
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
	    clear_pending_damage (qxl);
	    
	    /* We have to do this because the copy will cause the damage
	     * to be sent to move.
	     * 
	     * Instead of just sending the bits, we could also move
	     * the existing damage around; however that's a bit more 
	     * complex, and the performance win is unlikely to be
	     * very big.
	     */
	    qxl_send_copies (qxl);
	}
    
	while (n--)
	{
	    struct qxl_drawable *drawable;
	    struct qxl_rect qrect;
	    
	    qrect.top = b->y1;
	    qrect.bottom = b->y2;
	    qrect.left = b->x1;
	    qrect.right = b->x2;

/* 	    ErrorF ("   Translate %d %d %d %d by %d %d (offsets %d %d)\n", */
/* 		    b->x1, b->y1, b->x2, b->y2, */
/* 		    dx, dy, dst_xoff, dst_yoff); */
	    
	    drawable = make_drawable (qxl, QXL_COPY_BITS, &qrect);
	    drawable->u.copy_bits.src_pos.x = b->x1 + dx;
	    drawable->u.copy_bits.src_pos.y = b->y1 + dy;

	    push_drawable (qxl, drawable);

#if 0
	    if (closure)
		qxl_usleep (1000000);
#endif
	    
#if 0
	    submit_fill (qxl, &qrect, rand());
#endif

	    b++;
	}
    }
/*     else */
/* 	ErrorF ("Unaccelerated copy\n"); */

    fbCopyNtoN (pSrcDrawable, pDstDrawable, pGC, pbox, nbox, dx, dy, reverse, upsidedown, bitplane, closure);
}

static RegionPtr
qxl_copy_area(DrawablePtr pSrcDrawable, DrawablePtr pDstDrawable, GCPtr pGC,
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
			qxl_copy_n_to_n, 0, NULL);

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
qxl_fill_region_solid (DrawablePtr pDrawable, RegionPtr pRegion, Pixel pixel)
{
    ScreenPtr pScreen = pDrawable->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    PixmapPtr pPixmap;
    int xoff, yoff;

    if ((pPixmap = get_window_pixmap (pDrawable, &xoff, &yoff)))
    {
	int nbox = REGION_NUM_RECTS (pRegion);
	BoxPtr pBox = REGION_RECTS (pRegion);

	while (nbox--)
	{
	    struct qxl_rect qrect;

	    qrect.left = pBox->x1;
	    qrect.right = pBox->x2;
	    qrect.top = pBox->y1;
	    qrect.bottom = pBox->y2;

	    submit_fill (qxl, &qrect, pixel);

	    pBox++;
	}
    }

    fbFillRegionSolid (pDrawable, pRegion, 0,
		       fbReplicatePixel (pixel, pDrawable->bitsPerPixel));
}

static void
qxl_copy_window (WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)
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
		  &rgnDst, dx, dy, qxl_copy_n_to_n, 0, NULL);

    REGION_UNINIT (pScreen, &rgnDst);

/*     REGION_TRANSLATE (pScreen, prgnSrc, dx, dy); */
    
/*     fbCopyWindow (pWin, ptOldOrg, prgnSrc); */
}

static int
qxl_create_gc (GCPtr pGC)
{
    static GCOps ops;
    static int initialized;
    
    if (!fbCreateGC (pGC))
	return FALSE;

    if (!initialized)
    {
	ops = *pGC->ops;
	ops.PolyFillRect = qxl_poly_fill_rect;
	ops.CopyArea = qxl_copy_area;

	initialized = TRUE;
    }
    
    pGC->ops = &ops;
    return TRUE;
}

static Bool
qxl_screen_init(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    struct qxl_rom *rom;
    struct qxl_ram_header *ram_header;
    VisualPtr visual;

    CHECK_POINT();

    qxl->pScrn = pScrn;
    
    if (!qxl_map_memory(qxl, scrnIndex))
	return FALSE;

    rom = qxl->rom;
    ram_header = (void *)((unsigned long)qxl->ram + (unsigned long)qxl->rom->ram_header_offset);

    qxl_save_state(pScrn);
    qxl_blank_screen(pScreen, SCREEN_SAVER_ON);
    
    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
			  pScrn->rgbBits, pScrn->defaultVisual))
	goto out;
    if (!miSetPixmapDepths())
	goto out;

    /* Note we do this before setting pScrn->virtualY to match our current
       mode, so as to allocate a buffer large enough for the largest mode.
       FIXME: add support for resizing the framebuffer on modeset. */
    qxl->fb = xcalloc(pScrn->virtualY * pScrn->displayWidth, 4);
    if (!qxl->fb)
	goto out;

    pScrn->virtualX = pScrn->currentMode->HDisplay;
    pScrn->virtualY = pScrn->currentMode->VDisplay;
    
    if (!fbScreenInit(pScreen, qxl->fb,
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

    qxl->create_screen_resources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = qxl_create_screen_resources;

    /* Set up resources */
    qxl->mem = qxl_mem_create ((void *)((unsigned long)qxl->ram + (unsigned long)rom->pages_offset),
			       rom->num_io_pages * getpagesize());
    qxl->io_pages = (void *)((unsigned long)qxl->ram + (unsigned long)rom->pages_offset);
    qxl->io_pages_physical = (void *)((unsigned long)qxl->ram_physical + (unsigned long)rom->pages_offset);

    qxl->command_ring = qxl_ring_create (&(ram_header->cmd_ring_hdr),
					 sizeof (struct qxl_command),
					 32, qxl->io_base + QXL_IO_NOTIFY_CMD);
    qxl->cursor_ring = qxl_ring_create (&(ram_header->cursor_ring_hdr),
					sizeof (struct qxl_command),
					32, qxl->io_base + QXL_IO_NOTIFY_CURSOR);
    qxl->release_ring = qxl_ring_create (&(ram_header->release_ring_hdr),
					 sizeof (uint64_t),
					 8, 0);
					 
    /* xf86DPMSInit(pScreen, xf86DPMSSet, 0); */

#if 0 /* XV accel */
    qxlInitVideo(pScreen);
#endif

    pScreen->SaveScreen = qxl_blank_screen;
    qxl->close_screen = pScreen->CloseScreen;
    pScreen->CloseScreen = qxl_close_screen;

    qxl->create_gc = pScreen->CreateGC;
    pScreen->CreateGC = qxl_create_gc;

#if 0
    qxl->paint_window_background = pScreen->PaintWindowBackground;
    qxl->paint_window_border = pScreen->PaintWindowBorder;
#endif
    qxl->copy_window = pScreen->CopyWindow;
#if 0
    pScreen->PaintWindowBackground = qxl_paint_window;
    pScreen->PaintWindowBorder = qxl_paint_window;
#endif
    pScreen->CopyWindow = qxl_copy_window;

    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    if (!miCreateDefColormap(pScreen))
	goto out;

    qxl_cursor_init (pScreen);
    
    CHECK_POINT();

    qxl_switch_mode(scrnIndex, pScrn->currentMode, 0);

    CHECK_POINT();
    
    return TRUE;

out:
    return FALSE;
}

static Bool
qxl_enter_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

    qxl_save_state(pScrn);
    qxl_switch_mode(scrnIndex, pScrn->currentMode, 0);

    return TRUE;
}

static void
qxl_leave_vt(int scrnIndex, int flags)
{
    ScrnInfoPtr pScrn = xf86Screens[scrnIndex];

    qxl_restore_state(pScrn);
}

static Bool
qxl_color_setup(ScrnInfoPtr pScrn)
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
print_modes (qxl_screen_t *qxl, int scrnIndex)
{
    int i;

    for (i = 0; i < qxl->num_modes; ++i)
    {
	struct qxl_mode *m = qxl->modes + i;

	xf86DrvMsg (scrnIndex, X_INFO,
		    "%d: %dx%d, %d bits, stride %d, %dmm x %dmm, orientation %d\n",
		    m->id, m->x_res, m->y_res, m->bits, m->stride, m->x_mili,
		    m->y_mili, m->orientation);
    }
}

static Bool
qxl_check_device(ScrnInfoPtr pScrn, qxl_screen_t *qxl)
{
    int scrnIndex = pScrn->scrnIndex;
    struct qxl_rom *rom = qxl->rom;
    struct qxl_ram_header *ram_header = (void *)((unsigned long)qxl->ram + rom->ram_header_offset);

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

    qxl->draw_area_offset = rom->draw_area_offset;
    qxl->draw_area_size = rom->draw_area_size;
    pScrn->videoRam = rom->draw_area_size / 1024;
    
    return TRUE;
}

static int
qxl_find_native_mode(ScrnInfoPtr pScrn, DisplayModePtr p)
{
    int i;
    qxl_screen_t *qxl = pScrn->driverPrivate;

    CHECK_POINT();
    
    for (i = 0; i < qxl->num_modes; i++) 
    {
	struct qxl_mode *m = qxl->modes + i;

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
qxl_valid_mode(int scrn, DisplayModePtr p, Bool flag, int pass)
{
    ScrnInfoPtr pScrn = xf86Screens[scrn];
    qxl_screen_t *qxl = pScrn->driverPrivate;
    int bpp = pScrn->bitsPerPixel;
    int mode_idx;

    /* FIXME: I don't think this is necessary now that we report the
     * correct amount of video ram?
     */
    if (p->HDisplay * p->VDisplay * (bpp/8) > qxl->draw_area_size)
	return MODE_MEM;

    mode_idx = qxl_find_native_mode (pScrn, p);
    if (mode_idx == -1)
	return MODE_NOMODE;

    p->Private = (void *)(unsigned long)mode_idx;
    
    return MODE_OK;
}

static void qxl_add_mode(ScrnInfoPtr pScrn, int width, int height, int type)
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
qxl_pre_init(ScrnInfoPtr pScrn, int flags)
{
    int i, scrnIndex = pScrn->scrnIndex;
    qxl_screen_t *qxl = NULL;
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
	pScrn->driverPrivate = xnfcalloc(sizeof(qxl_screen_t), 1);
    qxl = pScrn->driverPrivate;
    
    qxl->entity = xf86GetEntityInfo(pScrn->entityList[0]);
    qxl->pci = xf86GetPciInfoForEntity(qxl->entity->index);
#ifndef XSERVER_LIBPCIACCESS
    qxl->pci_tag = pciTag(qxl->pci->bus, qxl->pci->device, qxl->pci->func);
#endif

    pScrn->monitor = pScrn->confScreen->monitor;

    if (!qxl_color_setup(pScrn))
	goto out;

    /* option parsing and card differentiation */
    xf86CollectOptions(pScrn, NULL);
    
    if (!qxl_map_memory(qxl, scrnIndex))
	goto out;

    if (!qxl_check_device(pScrn, qxl))
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
    for (i = 0; i < qxl->num_modes; i++)
        if (qxl->modes[i].orientation == 0) {
            qxl_add_mode(pScrn, qxl->modes[i].x_res, qxl->modes[i].y_res,
                         M_T_DRIVER);
            if (qxl->modes[i].x_res > max_x)
                max_x = qxl->modes[i].x_res;
            if (qxl->modes[i].y_res > max_y)
                max_y = qxl->modes[i].y_res;
        }

    if (pScrn->display->virtualX == 0 && pScrn->display->virtualY == 0) {
        /* It is possible for the largest x + largest y size combined leading
           to a virtual size which will not fit into the framebuffer when this
           happens we prefer max width and make height as large as possible */
        if (max_x * max_y * (pScrn->bitsPerPixel / 8) > qxl->draw_area_size)
            pScrn->display->virtualY = qxl->draw_area_size /
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

    print_modes (qxl, scrnIndex);

    /* VGA hardware initialisation */
    if (!vgaHWGetHWRec(pScrn))
        return FALSE;

    /* hate */
    qxl_unmap_memory(qxl, scrnIndex);

    CHECK_POINT();
    
    xf86DrvMsg(scrnIndex, X_INFO, "PreInit complete\n");
    return TRUE;

out:
    if (clockRanges)
	xfree(clockRanges);
    if (qxl)
	xfree(qxl);

    return FALSE;
}

#ifdef XSERVER_LIBPCIACCESS
enum qxl_class
{
    CHIP_QXL_1,
};

static const struct pci_id_match qxl_device_match[] = {
    {
	PCI_VENDOR_RED_HAT, PCI_CHIP_QXL_0100, PCI_MATCH_ANY, PCI_MATCH_ANY,
	0x00030000, 0x00ffffff, CHIP_QXL_1
    },

    { 0 },
};
#endif

static SymTabRec qxlChips[] =
{
    { PCI_CHIP_QXL_0100,	"QXL 1", },
    { -1, NULL }
};

#ifndef XSERVER_LIBPCIACCESS
static PciChipsets qxlPciChips[] =
{
    { PCI_CHIP_QXL_0100,    PCI_CHIP_QXL_0100,	RES_SHARED_VGA },
    { -1, -1, RES_UNDEFINED }
};
#endif

static void
qxl_identify(int flags)
{
    xf86PrintChipsets("qxl", "Driver for QXL virtual graphics", qxlChips);
}

static void
qxl_init_scrn(ScrnInfoPtr pScrn)
{
    pScrn->driverVersion    = 0;
    pScrn->driverName	    = pScrn->name = "qxl";
    pScrn->PreInit	    = qxl_pre_init;
    pScrn->ScreenInit	    = qxl_screen_init;
    pScrn->SwitchMode	    = qxl_switch_mode;
    pScrn->ValidMode	    = qxl_valid_mode;
    pScrn->EnterVT	    = qxl_enter_vt;
    pScrn->LeaveVT	    = qxl_leave_vt;
}
