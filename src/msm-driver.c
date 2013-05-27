/* msm-driver.c
 *
 * Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Code Aurora nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <sys/types.h>
#include <grp.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#include <linux/msm_kgsl.h>
#include <linux/msm_mdp.h>

#include "xf86.h"
#include "damage.h"
#include "xf86_OSlib.h"
#include "xf86Crtc.h"

#include "mipointer.h"
#include "micmap.h"
#include "fb.h"
#include "dixstruct.h"

#include "msm.h"
#include "msm-accel.h"
#include "compat-api.h"

#include <drm.h>
#include "xf86drm.h"

#define MSM_NAME        "freedreno"
#define MSM_DRIVER_NAME "freedreno"

#define MSM_VERSION_MAJOR PACKAGE_VERSION_MAJOR
#define MSM_VERSION_MINOR PACKAGE_VERSION_MINOR
#define MSM_VERSION_PATCH PACKAGE_VERSION_PATCHLEVEL

#define MSM_VERSION_CURRENT \
		((MSM_VERSION_MAJOR << 20) |\
				(MSM_VERSION_MINOR << 10) | \
				(MSM_VERSION_PATCH))


/* An aray containing the options that the user can
   configure in xorg.conf
 */

static const OptionInfoRec MSMOptions[] = {
		{OPTION_FB, "fb", OPTV_STRING, {0}, FALSE},
		{OPTION_NOACCEL, "NoAccel", OPTV_BOOLEAN, {0}, FALSE},
		{OPTION_SWCURSOR, "SWCursor", OPTV_BOOLEAN, {0}, FALSE},
		{OPTION_VSYNC, "DefaultVsync", OPTV_INTEGER, {0}, FALSE},
		{OPTION_DEBUG, "Debug", OPTV_BOOLEAN, {0}, FALSE},
		{-1, NULL, OPTV_NONE, {0}, FALSE}
};

// TODO make this a config option
Bool msmDebug = TRUE;

static void
MSMBlockHandler (BLOCKHANDLER_ARGS_DECL)
{
	SCREEN_PTR(arg);
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	MSMPtr pMsm = MSMPTR(pScrn);

	pScreen->BlockHandler = pMsm->BlockHandler;
	(*pScreen->BlockHandler) (BLOCKHANDLER_ARGS);
	pScreen->BlockHandler = MSMBlockHandler;

	if (pScrn->vtSema)
		MSMFlushAccel(pScreen);
}

/*
 * Because we don't use DRI1:
 */

static int
dri_drm_debug_print(const char *format, va_list ap)
{
	xf86VDrvMsgVerb(-1, X_NONE, 3, format, ap);
	return 0;
}

static void
dri_drm_get_perms(gid_t * group, mode_t * mode)
{
	*group = -1;
	*mode = 0666;
}

static drmServerInfo drm_server_info = {
	dri_drm_debug_print,
	xf86LoadKernelModule,
	dri_drm_get_perms,
};


static Bool
MSMInitDRM(ScrnInfoPtr pScrn)
{
	MSMPtr pMsm = MSMPTR(pScrn);

	drmSetServerInfo(&drm_server_info);

	pMsm->drmFD = drmOpen("kgsl", NULL);
	if (pMsm->drmFD < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				"Unable to open a DRM device\n");
		return FALSE;
	}

	pMsm->dev = fd_device_new(pMsm->drmFD);

	pMsm->deviceName = drmGetDeviceNameFromFd(pMsm->drmFD);

	return TRUE;
}

/* Get the current mode from the framebuffer mode and
 * convert it into xfree86 timings
 */

static void
MSMGetDefaultMode(MSMPtr pMsm)
{
	char name[32];
	sprintf(name, "%dx%d", pMsm->mode_info.xres, pMsm->mode_info.yres);

	pMsm->default_mode.name = strdup(name);

	if (pMsm->default_mode.name == NULL)
		pMsm->default_mode.name = "";

	pMsm->default_mode.next = &pMsm->default_mode;
	pMsm->default_mode.prev = &pMsm->default_mode;
	pMsm->default_mode.type |= M_T_BUILTIN | M_T_PREFERRED;

	pMsm->default_mode.HDisplay = pMsm->mode_info.xres;
	pMsm->default_mode.HSyncStart =
			pMsm->default_mode.HDisplay + pMsm->mode_info.right_margin;
	pMsm->default_mode.HSyncEnd =
			pMsm->default_mode.HSyncStart + pMsm->mode_info.hsync_len;
	pMsm->default_mode.HTotal =
			pMsm->default_mode.HSyncEnd + pMsm->mode_info.left_margin;

	pMsm->default_mode.VDisplay = pMsm->mode_info.yres;
	pMsm->default_mode.VSyncStart =
			pMsm->default_mode.VDisplay + pMsm->mode_info.lower_margin;
	pMsm->default_mode.VSyncEnd =
			pMsm->default_mode.VSyncStart + pMsm->mode_info.vsync_len;
	pMsm->default_mode.VTotal =
			pMsm->default_mode.VSyncEnd + pMsm->mode_info.upper_margin;

	/* The clock number we get is not the actual pixclock for the display,
	 * which automagically updates at a fixed rate.  There is no good way
	 * to automatically figure out the fixed rate, so we use a config
	 * value */

	pMsm->default_mode.Clock = (pMsm->defaultVsync *
			pMsm->default_mode.HTotal *
			pMsm->default_mode.VTotal) / 1000;

	pMsm->default_mode.CrtcHDisplay = pMsm->default_mode.HDisplay;
	pMsm->default_mode.CrtcHSyncStart = pMsm->default_mode.HSyncStart;
	pMsm->default_mode.CrtcHSyncEnd = pMsm->default_mode.HSyncEnd;
	pMsm->default_mode.CrtcHTotal = pMsm->default_mode.HTotal;

	pMsm->default_mode.CrtcVDisplay = pMsm->default_mode.VDisplay;
	pMsm->default_mode.CrtcVSyncStart = pMsm->default_mode.VSyncStart;
	pMsm->default_mode.CrtcVSyncEnd = pMsm->default_mode.VSyncEnd;
	pMsm->default_mode.CrtcVTotal = pMsm->default_mode.VTotal;

	pMsm->default_mode.CrtcHAdjusted = FALSE;
	pMsm->default_mode.CrtcVAdjusted = FALSE;
}

static Bool
MSMCrtcResize(ScrnInfoPtr pScrn, int width, int height)
{
	MSMPtr pMsm = MSMPTR(pScrn);
	int	oldx = pScrn->virtualX;
	int	oldy = pScrn->virtualY;
	ScreenPtr   screen = screenInfo.screens[pScrn->scrnIndex];
	if (oldx == width && oldy == height)
		return TRUE;
	pScrn->virtualX = width;
	pScrn->virtualY = height;
	pScrn->displayWidth = width;
	(*screen->ModifyPixmapHeader)((*screen->GetScreenPixmap)(screen),
			width, height, pScrn->depth, pScrn->bitsPerPixel, pScrn->displayWidth
			* (pScrn->bitsPerPixel / 8), NULL);
	pMsm->isFBSurfaceStale = TRUE;
	return TRUE;
}

static const xf86CrtcConfigFuncsRec MSMCrtcConfigFuncs = {
		MSMCrtcResize,
};

/* This is the main initialization function for the screen */

static Bool
MSMPreInit(ScrnInfoPtr pScrn, int flags)
{
	MSMPtr pMsm;
	EntityInfoPtr pEnt;
	const char *dev;
	int mdpver, panelid;
	int depth, fbbpp;
	rgb defaultWeight = { 0, 0, 0 };
	int vsync;

	DEBUG_MSG("pre-init");

	/* Omit ourselves from auto-probing (which is bound to
	 * fail on our hardware anyway)
	 */

	if (flags & PROBE_DETECT) {
		DEBUG_MSG("probe not supported");
		return FALSE;
	}

	if (pScrn->numEntities != 1) {
		DEBUG_MSG("numEntities=%d", pScrn->numEntities);
		return FALSE;
	}

	/* Just use the current monitor specified in the
	 * xorg.conf.  This really means little to us since
	 * we have no choice over which monitor is used,
	 * but X needs this to be set
	 */

	pScrn->monitor = pScrn->confScreen->monitor;

	/* Allocate room for our private data */
	if (pScrn->driverPrivate == NULL)
		pScrn->driverPrivate = xnfcalloc(sizeof(MSMRec), 1);

	pMsm = MSMPTR(pScrn);

	if (pMsm == NULL) {
		ERROR_MSG("Unable to allocate memory");
		return FALSE;
	}

	pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

	/* Open the FB device specified by the user */
	dev = xf86FindOptionValue(pEnt->device->options, "fb");

	pMsm->fd = open(dev, O_RDWR, 0);

	if (pMsm->fd < 0) {
		ERROR_MSG("Opening '%s' failed: %s", dev, strerror(errno));
		free(pMsm);
		return FALSE;
	}

	/* Unblank the screen if it was previously blanked */
	ioctl(pMsm->fd, FBIOBLANK, FB_BLANK_UNBLANK);

	/* Make sure the software refresher is on */
	ioctl(pMsm->fd, MSMFB_RESUME_SW_REFRESHER, 0);

	/* Get the fixed info (par) structure */

	if (ioctl(pMsm->fd, FBIOGET_FSCREENINFO, &pMsm->fixed_info)) {
		ERROR_MSG("Unable to read hardware info from %s: %s",
				dev, strerror(errno));
		free(pMsm);
		return FALSE;
	}

	/* Parse the ID and figure out what version of the MDP and what
	 * panel ID we have - default to the MDP3 */

	pMsm->chipID = MSM_MDP_VERSION_31;

	if (sscanf(pMsm->fixed_info.id, "msmfb%d_%x", &mdpver, &panelid) < 2) {
		WARNING_MSG("Unable to determine the MDP version - assume 3.1");
	}
	else {
		switch (mdpver) {
		case 22:
			pMsm->chipID = MSM_MDP_VERSION_22;
			break;
		case 31:
			pMsm->chipID = MSM_MDP_VERSION_31;
			break;
		case 40:
			pMsm->chipID = MSM_MDP_VERSION_40;
			break;
		default:
			WARNING_MSG("Unable to determine the MDP version - assume 3.1");
			break;
		}
	}

	/* FIXME:  If we want to parse the panel type, it happens here */

	/* Setup memory */

	/* FIXME:  This is where we will be in close communication with
	 * the fbdev driver to allocate memory.   In the mean time, we
	 * just reuse the framebuffer memory */

	pScrn->videoRam = pMsm->fixed_info.smem_len;

	/* Get the current screen setting */
	if (ioctl(pMsm->fd, FBIOGET_VSCREENINFO, &pMsm->mode_info)) {
		ERROR_MSG("Unable to read the current mode from %s: %s",
				dev, strerror(errno));

		free(pMsm);
		return FALSE;
	}

	/* msm-fb is made of fail.. need to pan otherwise backlight
	 * driver doesn't get kicked and we end up with backlight off.
	 * Makes perfect sense.
	 */
	pMsm->mode_info.yoffset = 1;
	if (ioctl(pMsm->fd, FBIOPAN_DISPLAY, &pMsm->mode_info)) {
		ERROR_MSG("could not pan on %s: %s", dev, strerror(errno));
	}
	/* we have to do this twice because if we were previously
	 * panned to offset 1, then the first FBIOPAN_DISPLAY wouldn't
	 * do anything.
	 */
	pMsm->mode_info.yoffset = 0;
	if (ioctl(pMsm->fd, FBIOPAN_DISPLAY, &pMsm->mode_info)) {
		ERROR_MSG("could not pan on %s: %s", dev, strerror(errno));
	}

	switch(pMsm->mode_info.bits_per_pixel) {
	case 16:
		depth = 16;
		fbbpp = 16;
		break;
	case 24:
	case 32:
		depth = 24;
		fbbpp = 32;
		break;
	default:
		ERROR_MSG("The driver can only support 16bpp and 24bpp output");
		free(pMsm);
		return FALSE;
	}

	if (!xf86SetDepthBpp(pScrn, depth, 0, fbbpp,
			Support24bppFb | Support32bppFb |
			SupportConvert32to24 | SupportConvert24to32)) {
		ERROR_MSG("Unable to set bitdepth");
		free(pMsm);
		return FALSE;
	}

	/* Set the color information in the mode structure to be set when the
      screen initializes.  This might seem like a redundant step, but
      at least on the 8650A, the default color setting is RGBA, not ARGB,
      so setting the color information here insures that the framebuffer
      mode is what we expect */

	switch(pScrn->depth) {
	case 16:
		pMsm->mode_info.bits_per_pixel = 16;
		pMsm->mode_info.red.offset = 11;
		pMsm->mode_info.green.offset = 5;
		pMsm->mode_info.blue.offset = 0;
		pMsm->mode_info.red.length = 5;
		pMsm->mode_info.green.length = 6;
		pMsm->mode_info.blue.length = 5;
		pMsm->mode_info.red.msb_right = 0;
		pMsm->mode_info.green.msb_right = 0;
		pMsm->mode_info.blue.msb_right = 0;
		pMsm->mode_info.transp.offset = 0;
		pMsm->mode_info.transp.length = 0;
		break;
	case 24:
	case 32:
		pMsm->mode_info.bits_per_pixel = 32;
		pMsm->mode_info.red.offset = 16;
		pMsm->mode_info.green.offset = 8;
		pMsm->mode_info.blue.offset = 0;
		pMsm->mode_info.blue.length = 8;
		pMsm->mode_info.green.length = 8;
		pMsm->mode_info.red.length = 8;
		pMsm->mode_info.blue.msb_right = 0;
		pMsm->mode_info.green.msb_right = 0;
		pMsm->mode_info.red.msb_right = 0;
		pMsm->mode_info.transp.offset = 24;
		pMsm->mode_info.transp.length = 8;
		break;
	default:
		ERROR_MSG("The driver can only support 16bpp and 24bpp output");
		free(pMsm);
		return FALSE;
	}

	xf86PrintDepthBpp(pScrn);
	pScrn->rgbBits = 8;

	if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight)) {
		free(pMsm);
		return FALSE;
	}

	/* Initialize default visual */
	if (!xf86SetDefaultVisual(pScrn, -1)) {
		free(pMsm);
		return FALSE;
	}

	{
		Gamma zeros = { 0.0, 0.0, 0.0 };

		if (!xf86SetGamma(pScrn, zeros)) {
			free(pMsm);
			return FALSE;
		}
	}

	pScrn->progClock = TRUE;
	pScrn->chipset = MSM_DRIVER_NAME;

	INFO_MSG("MSM/Qualcomm processor (video memory: %dkB)", pScrn->videoRam / 1024);

	xf86CollectOptions(pScrn, NULL);

	pMsm->options = malloc(sizeof(MSMOptions));

	if (pMsm->options == NULL) {
		free(pMsm);
		return FALSE;
	}

	memcpy(pMsm->options, MSMOptions, sizeof(MSMOptions));

	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, pMsm->options);

	/* Determine if the user wants debug messages turned on: */
	msmDebug = xf86ReturnOptValBool(pMsm->options, OPTION_DEBUG, FALSE);

	/* SWCursor - default FALSE */
	pMsm->HWCursor = !xf86ReturnOptValBool(pMsm->options, OPTION_SWCURSOR, FALSE);

	/* DefaultVsync - default 60 */
	pMsm->defaultVsync = 60;

	if (xf86GetOptValInteger(pMsm->options, OPTION_VSYNC, &vsync)) {
		if (vsync > 0 && vsync < 120)
			pMsm->defaultVsync = vsync;
	}

	if (!MSMInitDRM(pScrn)) {
		ERROR_MSG("Unable to open DRM");
		return FALSE;
	}

	/* Set up the virtual size */

	pScrn->virtualX = pScrn->display->virtualX > pMsm->mode_info.xres ?
			pScrn->display->virtualX : pMsm->mode_info.xres;

	pScrn->virtualY = pScrn->display->virtualY > pMsm->mode_info.yres ?
			pScrn->display->virtualY : pMsm->mode_info.yres;

	if (pScrn->virtualX > pMsm->mode_info.xres_virtual)
		pScrn->virtualX = pMsm->mode_info.xres_virtual;

	if (pScrn->virtualY > pMsm->mode_info.yres_virtual)
		pScrn->virtualY = pMsm->mode_info.yres_virtual;

	/* displayWidth is the width of the line in pixels */

	/* The framebuffer driver should always report the line length,
	 * but in case it doesn't, we can calculate it ourselves */

	if (pMsm->fixed_info.line_length) {
		pScrn->displayWidth = pMsm->fixed_info.line_length;
	} else {
		pScrn->displayWidth = pMsm->mode_info.xres_virtual *
				pMsm->mode_info.bits_per_pixel / 8;
	}

	pScrn->displayWidth /= (pScrn->bitsPerPixel / 8);

	/* Set up the view port */
	pScrn->frameX0 = 0;
	pScrn->frameY0 = 0;
	pScrn->frameX1 = pMsm->mode_info.xres;
	pScrn->frameY1 = pMsm->mode_info.yres;

	MSMGetDefaultMode(pMsm);

	/* Make a copy of the mode - this is important, because some
	 * where in the RandR setup, these modes get deleted */

	pScrn->modes = xf86DuplicateMode(&pMsm->default_mode);
	pScrn->currentMode = pScrn->modes;

	/* Set up the colors - this is from fbdevhw, which implies
	 * that it is important for TrueColor and DirectColor modes
	 */

	pScrn->offset.red = pMsm->mode_info.red.offset;
	pScrn->offset.green = pMsm->mode_info.green.offset;
	pScrn->offset.blue = pMsm->mode_info.blue.offset;

	pScrn->mask.red = ((1 << pMsm->mode_info.red.length) - 1)
			 << pMsm->mode_info.red.offset;

	pScrn->mask.green = ((1 << pMsm->mode_info.green.length) - 1)
			 << pMsm->mode_info.green.offset;

	pScrn->mask.blue = ((1 << pMsm->mode_info.blue.length) - 1)
			 << pMsm->mode_info.blue.offset;

	xf86CrtcConfigInit(pScrn, &MSMCrtcConfigFuncs);
	MSMCrtcSetup(pScrn);

	xf86CrtcSetSizeRange(pScrn,200,200,2048,2048);

	/* Setup the output */
	MSMOutputSetup(pScrn);

	if (!xf86InitialConfiguration(pScrn, FALSE)) {
		ERROR_MSG("configuration failed");
		free(pMsm);
		return FALSE;
	}

	xf86PrintModes(pScrn);

	/* FIXME:  We will probably need to be more exact when setting
	 * the DPI.  For now, we just use the default (96,96 I think) */

	xf86SetDpi(pScrn, 0, 0);

	INFO_MSG("MSM Options:");
	INFO_MSG(" HW Cursor: %s", pMsm->HWCursor ? "Enabled" : "Disabled");
	INFO_MSG(" Default Vsync: %d", pMsm->defaultVsync);

	return TRUE;
}

static Bool
MSMSaveScreen(ScreenPtr pScreen, int mode)
{
	/* Nothing to do here, yet */
	return TRUE;
}

static Bool
MSMCloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

	MSMPtr pMsm = MSMPTR(pScrn);

	DEBUG_MSG("close screen");

	/* Close DRI2 */
	if (pMsm->dri) {
		MSMDRI2CloseScreen(pScreen);
	}

	/* Close EXA */
	if (pMsm->pExa) {
		exaDriverFini(pScreen);
		free(pMsm->pExa);
		pMsm->pExa = NULL;
	}

	/* Unmap the framebuffer memory */
	munmap(pMsm->fbmem, pMsm->fixed_info.smem_len);

	pScreen->BlockHandler = pMsm->BlockHandler;
	pScreen->CloseScreen = pMsm->CloseScreen;

	return (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);
}

static Bool
MSMScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	MSMPtr pMsm = MSMPTR(pScrn);

	DEBUG_MSG("screen-init");

#if 0
#if defined (MSMFB_GET_PAGE_PROTECTION) && defined (MSMFB_SET_PAGE_PROTECTION)
	/* If the frame buffer can be cached, do so.                                      */
	/* CAUTION: This needs to be done *BEFORE* the mmap() call, or it has no effect.  */
	/* FIXME:  The current page protection should ideally be saved here and restored  */
	/*         when exiting the driver, but there may be little point in doing this   */
	/*         since the XServer typically won't exit for most applications.          */
	{
		const int desired_fb_page_protection = pMsm->FBCache;
		struct mdp_page_protection fb_page_protection;

		// If the kernel supports the FB Caching settings infrastructure,
		// then set the frame buffer cache settings.
		// Otherwise, issue a warning and continue.
		if (ioctl(pMsm->fd, MSMFB_GET_PAGE_PROTECTION, &fb_page_protection)) {
			xf86DrvMsg(scrnIndex, X_WARNING,
					"MSMFB_GET_PAGE_PROTECTION IOCTL: Unable to get current FB cache settings.\n");
		}
		else {
			if (fb_page_protection.page_protection != desired_fb_page_protection) {
				fb_page_protection.page_protection = desired_fb_page_protection;
				if (ioctl(pMsm->fd, MSMFB_SET_PAGE_PROTECTION, &fb_page_protection)) {
					xf86DrvMsg(scrnIndex, X_WARNING,
							"MSMFB_SET_PAGE_PROTECTION IOCTL: Unable to set requested FB cache settings: %s.\n",
							fbCacheStrings[desired_fb_page_protection]);
				}
			}
		}
	}
#endif // defined (MSMFB_GET_PAGE_PROTECTION) && defined (MSMFB_SET_PAGE_PROTECTION)
#endif

	/* Map the framebuffer memory */
	pMsm->fbmem = mmap(NULL, pMsm->fixed_info.smem_len,
			PROT_READ | PROT_WRITE, MAP_SHARED, pMsm->fd, 0);

	/* If we can't map the memory, then this is a short trip */

	if (pMsm->fbmem == MAP_FAILED) {
		ERROR_MSG("Unable to map the framebuffer memory: %s", strerror(errno));
		return FALSE;
	}

	/* Set up the mode - this doesn't actually touch the hardware,
	 * but it makes RandR all happy */

	if (!xf86SetDesiredModes(pScrn)) {
		ERROR_MSG("Unable to set the mode");
		return FALSE;
	}

	/* Set up the X visuals */
	miClearVisualTypes();

	/* We only support TrueColor at the moment, and I suspect that is all
	 * we will ever support */

	if (!miSetVisualTypes(pScrn->depth, TrueColorMask,
			pScrn->rgbBits, TrueColor)) {
		ERROR_MSG("Unable to set up the visual for %d BPP", pScrn->bitsPerPixel);
		return FALSE;
	}

	if (!miSetPixmapDepths()) {
		ERROR_MSG("Unable to set the pixmap depth");
		return FALSE;
	}

	/* Set up the X drawing area */

	xf86LoadSubModule(pScrn, "fb");

	if (!fbScreenInit(pScreen, pMsm->fbmem,
			pScrn->virtualX, pScrn->virtualY,
			pScrn->xDpi, pScrn->yDpi,
			pScrn->displayWidth, pScrn->bitsPerPixel)) {
		ERROR_MSG("fbScreenInit failed");
		return FALSE;
	}

	/* Set up the color information for the visual(s) */

	if (pScrn->bitsPerPixel > 8) {
		VisualPtr visual = pScreen->visuals + pScreen->numVisuals;

		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue = pScrn->offset.blue;
				visual->redMask = pScrn->mask.red;
				visual->greenMask = pScrn->mask.green;
				visual->blueMask = pScrn->mask.blue;
			}
		}
	}

	/* Set up the Render fallbacks */
	if (!fbPictureInit(pScreen, NULL, 0)) {
		ERROR_MSG("fbPictureInit failed");
		return FALSE;
	}

	/* Set default colors */
	xf86SetBlackWhitePixels(pScreen);

	/* Set up the backing store */
	xf86SetBackingStore(pScreen);

	/* Set up EXA */
	xf86LoadSubModule(pScrn, "exa");

	if (!MSMSetupAccel(pScreen))
		ERROR_MSG("Unable to setup EXA");

	/* Set up the software cursor */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	/* Try to set up the HW cursor */

	if (pMsm->HWCursor == TRUE)
		pMsm->HWCursor = MSMCursorInit(pScreen);

	/* Set up the default colormap */

	if (!miCreateDefColormap(pScreen)) {
		ERROR_MSG("miCreateDefColormap failed");
		return FALSE;
	}

	/* FIXME: Set up DPMS here */

	pScreen->SaveScreen = MSMSaveScreen;

	/*Set up our own CloseScreen function */

	pMsm->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = MSMCloseScreen;

	pMsm->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = MSMBlockHandler;

	if (!xf86CrtcScreenInit(pScreen)) {
		ERROR_MSG("CRTCScreenInit failed");
		return FALSE;
	}

	return TRUE;
}

static Bool
MSMSwitchMode(SWITCH_MODE_ARGS_DECL)
{
	/* FIXME:  We should only have the one mode, so we shouldn't ever call
	 * this function - regardless, it needs to be stubbed - so what
	 * do we return, TRUE or FALSE? */

	return TRUE;
}

static Bool
MSMEnterVT(VT_FUNC_ARGS_DECL)
{
	/* Nothing to do here yet - there might be some triggers that we need
	 * to throw at the framebuffer */
	return TRUE;
}

static void
MSMLeaveVT(VT_FUNC_ARGS_DECL)
{
}

/* ------------------------------------------------------------ */
/* Following is the standard driver setup that probes for the   */
/* hardware and sets up the structures.                         */

static const OptionInfoRec *
MSMAvailableOptions(int chipid, int busid)
{
	return MSMOptions;
}

static void
MSMIdentify(int flags)
{
	xf86Msg(X_INFO, "%s: Video driver for Qualcomm processors\n", MSM_NAME);
}

static Bool
MSMProbe(DriverPtr drv, int flags)
{
	GDevPtr *sections;
	int nsects;
	const char *dev;

	Bool foundScreen = FALSE;

	ScrnInfoPtr pScrn = NULL;

	int fd, i;

	/* For now, just return false during a probe */

	if (flags & PROBE_DETECT) {
		ErrorF("probe not supported\n");
		return FALSE;
	}

	/* Find all of the device sections in the config */

	nsects = xf86MatchDevice(MSM_NAME, &sections);
	if (nsects <= 0) {
		ErrorF("nsects=%d\n", nsects);
		return FALSE;
	}

	/* We know that we will only have at most 4 possible outputs */

	for (i = 0; i < (nsects > 4 ? 4 : nsects); i++) {

		dev = xf86FindOptionValue(sections[i]->options, "fb");

		xf86Msg(X_WARNING, "Section %d - looking for %s\n", i, dev);

		/* FIXME:  There should be some discussion about how we
		 * refer to devices - blindly matching to /dev/fbX files
		 * seems like it could backfire on us.   For now, force
		 * the user to set the backing FB in the xorg.conf */

		if (dev == NULL) {
			xf86Msg(X_WARNING, "no device specified in section %d\n", i);
			continue;
		}

		fd = open(dev, O_RDWR, 0);

		if (fd <= 0) {
			xf86Msg(X_WARNING, "Could not open '%s': %s\n",
					dev, strerror(errno));
			continue;
		} else {
			struct fb_fix_screeninfo info;

			int entity;

			if (ioctl(fd, FBIOGET_FSCREENINFO, &info)) {
				xf86Msg(X_WARNING,
						"Unable to read hardware info "
						"from %s: %s\n", dev, strerror(errno));
				close(fd);
				continue;
			}

			close(fd);

			/* Make sure that this is a MSM driver */
			if (strncmp(info.id, "msmfb", 5)) {
				xf86Msg(X_WARNING, "%s is not a MSM device: %s\n", dev, info.id);
				continue;
			}

			foundScreen = TRUE;

			entity = xf86ClaimFbSlot(drv, 0, sections[i], TRUE);
			pScrn = xf86ConfigFbEntity(NULL, 0, entity, NULL, NULL, NULL, NULL);

			xf86Msg(X_WARNING, "Add screen %p\n", pScrn);

			/* Set up the hooks for the screen */

			pScrn->driverVersion = MSM_VERSION_CURRENT;
			pScrn->driverName = MSM_NAME;
			pScrn->name = MSM_NAME;
			pScrn->Probe = MSMProbe;
			pScrn->PreInit = MSMPreInit;
			pScrn->ScreenInit = MSMScreenInit;
			pScrn->SwitchMode = MSMSwitchMode;
			pScrn->EnterVT = MSMEnterVT;
			pScrn->LeaveVT = MSMLeaveVT;
		}
	}

	free(sections);
	return foundScreen;
}

_X_EXPORT DriverRec freedrenoDriver = {
		MSM_VERSION_CURRENT,
		MSM_DRIVER_NAME,
		MSMIdentify,
		MSMProbe,
		MSMAvailableOptions,
		NULL,
		0,
		NULL
};

MODULESETUPPROTO(freedrenoSetup);

/* Versioning information for the module - most of these variables will
   come from config.h generated by ./configure
 */

static XF86ModuleVersionInfo freedrenoVersRec = {
		MSM_DRIVER_NAME,
		MODULEVENDORSTRING,
		MODINFOSTRING1,
		MODINFOSTRING2,
		XORG_VERSION_CURRENT,
		MSM_VERSION_MAJOR, MSM_VERSION_MINOR, MSM_VERSION_PATCH,
		ABI_CLASS_VIDEODRV,
		ABI_VIDEODRV_VERSION,
		NULL,
		{0, 0, 0, 0},
};

_X_EXPORT XF86ModuleData freedrenoModuleData = { &freedrenoVersRec, freedrenoSetup, NULL };

pointer
freedrenoSetup(pointer module, pointer ops, int *errmaj, int *errmin)
{
	static Bool initDone = FALSE;

	if (initDone == FALSE) {
		initDone = TRUE;
		xf86AddDriver(&freedrenoDriver, module, HaveDriverFuncs);

		/* FIXME: Load symbol references here */
		return (pointer) 1;
	} else {
		if (errmaj)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
}
