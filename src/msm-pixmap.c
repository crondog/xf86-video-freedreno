/* msm-pixmap.c
 *
 * Copyright (c) 2009 - 2010 Code Aurora Forum. All rights reserved.
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

#include "msm.h"


struct fd_bo *
msm_get_pixmap_bo(PixmapPtr pix)
{
	struct msm_pixmap_priv *priv = exaGetPixmapDriverPrivate(pix);

	if (priv && priv->bo)
		return priv->bo;

	/* TODO: perhaps the special handling for scanout pixmap should be done
	 * elsewhere so it isn't in a hot path.. ie. when the scanout buffer is
	 * allocated..
	 */
	if (priv) {
		ScreenPtr pScreen = pix->drawable.pScreen;
		MSMPtr pMsm = MSMPTR_FROM_PIXMAP(pix);
		// TODO .. how to handle offset for rotated pixmap.. worry about that later
		if (pScreen->GetScreenPixmap(pScreen) == pix) {
			priv->bo = pMsm->scanout;
			return priv->bo;
		}
	}

	return NULL;
}

int
msm_get_pixmap_name(PixmapPtr pix, unsigned int *name, unsigned int *pitch)
{
	int ret = -1;
	struct fd_bo *bo = msm_get_pixmap_bo(pix);
	if (bo) {
		*pitch = exaGetPixmapPitch(pix);
		ret = fd_bo_get_name(bo, name);
	}
	return ret;
}
