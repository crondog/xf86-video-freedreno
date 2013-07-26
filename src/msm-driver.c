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


static Bool MSMEnterVT(VT_FUNC_ARGS_DECL);
static void MSMLeaveVT(VT_FUNC_ARGS_DECL);

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
		FIRE_RING(pMsm);
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

/* This is the main initialization function for the screen */

static Bool
MSMPreInit(ScrnInfoPtr pScrn, int flags)
{
	MSMPtr pMsm;
	rgb defaultWeight = { 0, 0, 0 };
	Gamma zeros = { 0.0, 0.0, 0.0 };

	DEBUG_MSG("pre-init");

	/* Omit ourselves from auto-probing (which is bound to
	 * fail on our hardware anyway)
	 *
	 * TODO we could probe for drm device..
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

	xf86PrintDepthBpp(pScrn);
	pScrn->rgbBits = 8;

	pScrn->progClock = TRUE;
	pScrn->chipset = MSM_DRIVER_NAME;

	INFO_MSG("MSM/Qualcomm processor (video memory: %dkB)", pScrn->videoRam / 1024);

	if (!MSMInitDRM(pScrn)) {
		ERROR_MSG("Unable to open DRM");
		return FALSE;
	}

	if (!fbmode_pre_init(pScrn)) {
		ERROR_MSG("fbdev modesetting failed to initialize");
		return FALSE;
	}

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

	xf86PrintModes(pScrn);

	/* FIXME:  We will probably need to be more exact when setting
	 * the DPI.  For now, we just use the default (96,96 I think) */

	xf86SetDpi(pScrn, 0, 0);

	if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight)) {
		free(pMsm);
		return FALSE;
	}

	/* Initialize default visual */
	if (!xf86SetDefaultVisual(pScrn, -1)) {
		free(pMsm);
		return FALSE;
	}

	if (!xf86SetGamma(pScrn, zeros)) {
		free(pMsm);
		return FALSE;
	}

	INFO_MSG("MSM Options:");
	INFO_MSG(" HW Cursor: %s", pMsm->HWCursor ? "Enabled" : "Disabled");

	return TRUE;
}

static Bool
MSMSaveScreen(ScreenPtr pScreen, int mode)
{
	/* Nothing to do here, yet */
	return TRUE;
}

static Bool
MSMCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	MSMPtr pMsm = MSMPTR(pScrn);
	PixmapPtr ppix;

	pScreen->CreateScreenResources = pMsm->CreateScreenResources;
	if (!(*pScreen->CreateScreenResources)(pScreen))
		return FALSE;
	pScreen->CreateScreenResources = MSMCreateScreenResources;

	if (!MSMEnterVT(VT_FUNC_ARGS(0)))
		return FALSE;

	ppix = pScreen->GetScreenPixmap(pScreen);
	if (ppix)
		msm_set_pixmap_bo(ppix, pMsm->scanout);

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

	if (pScrn->vtSema) {
		MSMLeaveVT(VT_FUNC_ARGS(0));
		pScrn->vtSema = FALSE;
	}

	fbmode_screen_fini(pScreen);

	pScreen->BlockHandler = pMsm->BlockHandler;
	pScreen->CloseScreen = pMsm->CloseScreen;

	return (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);
}

static Bool
MSMScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	MSMPtr pMsm = MSMPTR(pScrn);
	int displayWidth;

	DEBUG_MSG("screen-init");

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

	displayWidth = pScrn->displayWidth;
	if (!displayWidth)
		displayWidth = pScrn->virtualX;

	xf86LoadSubModule(pScrn, "fb");

	if (!fbScreenInit(pScreen, NULL,
			pScrn->virtualX, pScrn->virtualY,
			pScrn->xDpi, pScrn->yDpi,
			displayWidth, pScrn->bitsPerPixel)) {
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
	if (pMsm->HWCursor) {
		pMsm->HWCursor = fbmode_cursor_init(pScreen);

		if (!pMsm->HWCursor)
			ERROR_MSG("Hardware cursor initialization failed");
	}

	/* Set up the default colormap */

	if (!miCreateDefColormap(pScreen)) {
		ERROR_MSG("miCreateDefColormap failed");
		return FALSE;
	}

	pScreen->SaveScreen = MSMSaveScreen;

	pMsm->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = MSMCloseScreen;

	pMsm->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = MSMCreateScreenResources;

	pMsm->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = MSMBlockHandler;

	if (!xf86CrtcScreenInit(pScreen)) {
		ERROR_MSG("CRTCScreenInit failed");
		return FALSE;
	}

	if (!fbmode_screen_init(pScreen)) {
		ERROR_MSG("fbmode_screen_init failed");
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
	SCRN_INFO_PTR(arg);
	MSMPtr pMsm = MSMPTR(pScrn);

	DEBUG_MSG("enter-vt");

	/* Set up the mode - this doesn't actually touch the hardware,
	 * but it makes RandR all happy */

	if (!xf86SetDesiredModes(pScrn)) {
		ERROR_MSG("Unable to set the mode");
		return FALSE;
	}

	return TRUE;
}

static void
MSMLeaveVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	MSMPtr pMsm = MSMPTR(pScrn);

	DEBUG_MSG("leave-vt");
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
			int entity, fd;

			fd = drmOpen("kgsl", NULL);

			if (fd < 0)
				continue;

			close(fd);

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
