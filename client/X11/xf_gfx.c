/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 Graphics Pipeline
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf_gfx.h"

int xf_ResetGraphics(RdpgfxClientContext* context, RDPGFX_RESET_GRAPHICS_PDU* resetGraphics)
{
	xfContext* xfc = (xfContext*) context->custom;

	printf("xf_ResetGraphics: width: %d height: %d\n",
			resetGraphics->width, resetGraphics->height);

	if (xfc->rfx)
	{
		rfx_context_free(xfc->rfx);
		xfc->rfx = NULL;
	}

	xfc->rfx = rfx_context_new(FALSE);

	xfc->rfx->width = resetGraphics->width;
	xfc->rfx->height = resetGraphics->height;
	rfx_context_set_pixel_format(xfc->rfx, RDP_PIXEL_FORMAT_B8G8R8A8);

	if (xfc->nsc)
	{
		nsc_context_free(xfc->nsc);
		xfc->nsc = NULL;
	}

	xfc->nsc = nsc_context_new();

	xfc->nsc->width = resetGraphics->width;
	xfc->nsc->height = resetGraphics->height;
	nsc_context_set_pixel_format(xfc->nsc, RDP_PIXEL_FORMAT_B8G8R8A8);

	xfc->clear = clear_context_new(FALSE);

	region16_init(&(xfc->invalidRegion));

	xfc->graphicsReset = TRUE;

	return 1;
}

int xf_OutputUpdate(xfContext* xfc)
{
	UINT16 width, height;
	xfGfxSurface* surface;
	RECTANGLE_16 surfaceRect;
	const RECTANGLE_16* extents;

	if (!xfc->graphicsReset)
		return 1;

	surface = (xfGfxSurface*) xfc->gfx->GetSurfaceData(xfc->gfx, xfc->outputSurfaceId);

	if (!surface)
		return -1;

	surfaceRect.left = 0;
	surfaceRect.top = 0;
	surfaceRect.right = xfc->width;
	surfaceRect.bottom = xfc->height;

	region16_intersect_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &surfaceRect);

	XSetClipMask(xfc->display, xfc->gc, None);
	XSetFunction(xfc->display, xfc->gc, GXcopy);
	XSetFillStyle(xfc->display, xfc->gc, FillSolid);

	if (!region16_is_empty(&(xfc->invalidRegion)))
	{
		extents = region16_extents(&(xfc->invalidRegion));

		width = extents->right - extents->left;
		height = extents->bottom - extents->top;

		if (width > xfc->width)
			width = xfc->width;

		if (height > xfc->height)
			height = xfc->height;

		XPutImage(xfc->display, xfc->drawable, xfc->gc, surface->image,
				extents->left, extents->top,
				extents->left, extents->top, width, height);
	}

	region16_clear(&(xfc->invalidRegion));

	XSetClipMask(xfc->display, xfc->gc, None);
	XSync(xfc->display, True);

	return 1;
}

int xf_OutputExpose(xfContext* xfc, int x, int y, int width, int height)
{
	RECTANGLE_16 invalidRect;

	invalidRect.left = x;
	invalidRect.top = y;
	invalidRect.right = x + width;
	invalidRect.bottom = y + height;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	xf_OutputUpdate(xfc);

	return 1;
}

int xf_StartFrame(RdpgfxClientContext* context, RDPGFX_START_FRAME_PDU* startFrame)
{
	xfContext* xfc = (xfContext*) context->custom;

	xfc->inGfxFrame = TRUE;

	return 1;
}

int xf_EndFrame(RdpgfxClientContext* context, RDPGFX_END_FRAME_PDU* endFrame)
{
	xfContext* xfc = (xfContext*) context->custom;

	xf_OutputUpdate(xfc);

	xfc->inGfxFrame = FALSE;

	return 1;
}

int xf_SurfaceCommand_Uncompressed(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	freerdp_image_copy(surface->data, PIXEL_FORMAT_XRGB32, surface->scanline, cmd->left, cmd->top,
			cmd->width, cmd->height, cmd->data, PIXEL_FORMAT_XRGB32, cmd->width * 4, 0, 0);

	invalidRect.left = cmd->left;
	invalidRect.top = cmd->top;
	invalidRect.right = cmd->right;
	invalidRect.bottom = cmd->bottom;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_RemoteFX(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int j;
	UINT16 i;
	RFX_RECT* rect;
	RFX_TILE* tile;
	int nXDst, nYDst;
	int nWidth, nHeight;
	int nbUpdateRects;
	RFX_MESSAGE* message;
	xfGfxSurface* surface;
	REGION16 updateRegion;
	RECTANGLE_16 updateRect;
	RECTANGLE_16* updateRects;
	REGION16 clippingRects;
	RECTANGLE_16 clippingRect;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	message = rfx_process_message(xfc->rfx, cmd->data, cmd->length);

	if (!message)
		return -1;

	region16_init(&clippingRects);

	for (i = 0; i < message->numRects; i++)
	{
		rect = &(message->rects[i]);

		clippingRect.left = cmd->left + rect->x;
		clippingRect.top = cmd->top + rect->y;
		clippingRect.right = clippingRect.left + rect->width;
		clippingRect.bottom = clippingRect.top + rect->height;

		region16_union_rect(&clippingRects, &clippingRects, &clippingRect);
	}

	for (i = 0; i < message->numTiles; i++)
	{
		tile = message->tiles[i];

		updateRect.left = cmd->left + tile->x;
		updateRect.top = cmd->top + tile->y;
		updateRect.right = updateRect.left + 64;
		updateRect.bottom = updateRect.top + 64;

		region16_init(&updateRegion);
		region16_intersect_rect(&updateRegion, &clippingRects, &updateRect);
		updateRects = (RECTANGLE_16*) region16_rects(&updateRegion, &nbUpdateRects);

		for (j = 0; j < nbUpdateRects; j++)
		{
			nXDst = updateRects[j].left;
			nYDst = updateRects[j].top;
			nWidth = updateRects[j].right - updateRects[j].left;
			nHeight = updateRects[j].bottom - updateRects[j].top;

			freerdp_image_copy(surface->data, PIXEL_FORMAT_XRGB32, surface->scanline,
					nXDst, nYDst, nWidth, nHeight,
					tile->data, PIXEL_FORMAT_XRGB32, 64 * 4, 0, 0);

			region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &updateRects[j]);
		}

		region16_uninit(&updateRegion);
	}

	rfx_message_free(xfc->rfx, message);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_ClearCodec(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status;
	UINT32 DstSize = 0;
	BYTE* pDstData = NULL;
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	status = clear_decompress(xfc->clear, cmd->data, cmd->length, &pDstData, &DstSize);

	printf("xf_SurfaceCommand_ClearCodec: status: %d\n", status);

	/* fill with pink for now to distinguish from the rest */

	freerdp_image_fill(surface->data, PIXEL_FORMAT_XRGB32, surface->scanline,
			cmd->left, cmd->top, cmd->width, cmd->height, 0xFF69B4);

	invalidRect.left = cmd->left;
	invalidRect.top = cmd->top;
	invalidRect.right = cmd->right;
	invalidRect.bottom = cmd->bottom;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_Planar(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status;
	BYTE* DstData = NULL;
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	DstData = surface->data;

	status = planar_decompress(NULL, cmd->data, cmd->length, &DstData,
			PIXEL_FORMAT_XRGB32, surface->scanline, cmd->left, cmd->top, cmd->width, cmd->height);

	invalidRect.left = cmd->left;
	invalidRect.top = cmd->top;
	invalidRect.right = cmd->right;
	invalidRect.bottom = cmd->bottom;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand(RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status = 1;
	xfContext* xfc = (xfContext*) context->custom;

	switch (cmd->codecId)
	{
		case RDPGFX_CODECID_UNCOMPRESSED:
			status = xf_SurfaceCommand_Uncompressed(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_CAVIDEO:
			status = xf_SurfaceCommand_RemoteFX(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_CLEARCODEC:
			status = xf_SurfaceCommand_ClearCodec(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_PLANAR:
			status = xf_SurfaceCommand_Planar(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_H264:
			printf("xf_SurfaceCommand_H264\n");
			break;

		case RDPGFX_CODECID_ALPHA:
			printf("xf_SurfaceCommand_Alpha\n");
			break;

		case RDPGFX_CODECID_CAPROGRESSIVE:
			printf("xf_SurfaceCommand_Progressive\n");
			break;

		case RDPGFX_CODECID_CAPROGRESSIVE_V2:
			printf("xf_SurfaceCommand_ProgressiveV2\n");
			break;
	}

	return 1;
}

int xf_DeleteEncodingContext(RdpgfxClientContext* context, RDPGFX_DELETE_ENCODING_CONTEXT_PDU* deleteEncodingContext)
{
	printf("xf_DeleteEncodingContext\n");

	return 1;
}

int xf_CreateSurface(RdpgfxClientContext* context, RDPGFX_CREATE_SURFACE_PDU* createSurface)
{
	xfGfxSurface* surface;
	xfContext* xfc = (xfContext*) context->custom;

	printf("xf_CreateSurface: surfaceId: %d width: %d height: %d format: 0x%02X\n",
			createSurface->surfaceId, createSurface->width, createSurface->height, createSurface->pixelFormat);

	surface = (xfGfxSurface*) calloc(1, sizeof(xfGfxSurface));

	if (!surface)
		return -1;

	surface->surfaceId = createSurface->surfaceId;
	surface->width = (UINT32) createSurface->width;
	surface->height = (UINT32) createSurface->height;
	surface->alpha = (createSurface->pixelFormat == PIXEL_FORMAT_ARGB_8888) ? TRUE : FALSE;

	surface->scanline = (surface->width + (surface->width % 4)) * 4;
	surface->data = (BYTE*) calloc(1, surface->scanline * surface->height);

	if (!surface->data)
		return -1;

	surface->image = XCreateImage(xfc->display, xfc->visual, 24, ZPixmap, 0,
			(char*) surface->data, surface->width, surface->height, 32, surface->scanline);

	context->SetSurfaceData(context, surface->surfaceId, (void*) surface);

	return 1;
}

int xf_DeleteSurface(RdpgfxClientContext* context, RDPGFX_DELETE_SURFACE_PDU* deleteSurface)
{
	xfGfxSurface* surface = NULL;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, deleteSurface->surfaceId);

	printf("xf_DeleteSurface: surfaceId: %d\n", deleteSurface->surfaceId);

	if (surface)
	{
		XFree(surface->image);
		free(surface->data);
		free(surface);
	}

	context->SetSurfaceData(context, deleteSurface->surfaceId, NULL);

	return 1;
}

int xf_SolidFill(RdpgfxClientContext* context, RDPGFX_SOLID_FILL_PDU* solidFill)
{
	UINT16 index;
	UINT32 color;
	BYTE a, r, g, b;
	int nWidth, nHeight;
	RDPGFX_RECT16* rect;
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;
	xfContext* xfc = (xfContext*) context->custom;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, solidFill->surfaceId);

	printf("xf_SolidFill\n");

	if (!surface)
		return -1;

	b = solidFill->fillPixel.B;
	g = solidFill->fillPixel.G;
	r = solidFill->fillPixel.R;
	a = solidFill->fillPixel.XA;

	color = ARGB32(a, r, g, b);

	for (index = 0; index < solidFill->fillRectCount; index++)
	{
		rect = &(solidFill->fillRects[index]);

		nWidth = rect->right - rect->left;
		nHeight = rect->bottom - rect->top;

		invalidRect.left = rect->left;
		invalidRect.top = rect->top;
		invalidRect.right = rect->right;
		invalidRect.bottom = rect->bottom;

		freerdp_image_fill(surface->data, PIXEL_FORMAT_XRGB32, surface->scanline,
				rect->left, rect->top, nWidth, nHeight, color);

		region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);
	}

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceToSurface(RdpgfxClientContext* context, RDPGFX_SURFACE_TO_SURFACE_PDU* surfaceToSurface)
{
	UINT16 index;
	int nWidth, nHeight;
	RDPGFX_RECT16* rectSrc;
	RDPGFX_POINT16* destPt;
	RECTANGLE_16 invalidRect;
	xfGfxSurface* surfaceSrc;
	xfGfxSurface* surfaceDst;
	xfContext* xfc = (xfContext*) context->custom;

	rectSrc = &(surfaceToSurface->rectSrc);
	destPt = &surfaceToSurface->destPts[0];

	surfaceSrc = (xfGfxSurface*) context->GetSurfaceData(context, surfaceToSurface->surfaceIdSrc);

	if (surfaceToSurface->surfaceIdSrc != surfaceToSurface->surfaceIdDest)
		surfaceDst = (xfGfxSurface*) context->GetSurfaceData(context, surfaceToSurface->surfaceIdDest);
	else
		surfaceDst = surfaceSrc;

	if (!surfaceSrc || !surfaceDst)
		return -1;

	nWidth = rectSrc->right - rectSrc->left + 1;
	nHeight = rectSrc->bottom - rectSrc->top + 1;

	for (index = 0; index < surfaceToSurface->destPtsCount; index++)
	{
		destPt = &surfaceToSurface->destPts[index];

		freerdp_image_copy(surfaceDst->data, PIXEL_FORMAT_XRGB32, surfaceDst->scanline,
				destPt->x, destPt->y, nWidth, nHeight, surfaceSrc->data, PIXEL_FORMAT_XRGB32,
				surfaceSrc->scanline, rectSrc->left, rectSrc->top);

		invalidRect.left = destPt->x;
		invalidRect.top = destPt->y;
		invalidRect.right = destPt->x + rectSrc->right;
		invalidRect.bottom = destPt->y + rectSrc->bottom;

		region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);
	}

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceToCache(RdpgfxClientContext* context, RDPGFX_SURFACE_TO_CACHE_PDU* surfaceToCache)
{
	RDPGFX_RECT16* rect;
	xfGfxSurface* surface;
	xfGfxCacheEntry* cacheEntry;

	rect = &(surfaceToCache->rectSrc);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, surfaceToCache->surfaceId);

	//printf("xf_SurfaceToCache: cacheKey: 0x%016X cacheSlot: %ld\n",
	//		surfaceToCache->cacheKey, surfaceToCache->cacheSlot);

	if (!surface)
		return -1;

	cacheEntry = (xfGfxCacheEntry*) calloc(1, sizeof(xfGfxCacheEntry));

	if (!cacheEntry)
		return -1;

	cacheEntry->width = (UINT32) (rect->right - rect->left);
	cacheEntry->height = (UINT32) (rect->bottom - rect->top);
	cacheEntry->alpha = surface->alpha;

	cacheEntry->scanline = (cacheEntry->width + (cacheEntry->width % 4)) * 4;
	cacheEntry->data = (BYTE*) calloc(1, surface->scanline * surface->height);

	if (!cacheEntry->data)
		return -1;

	freerdp_image_copy(cacheEntry->data, PIXEL_FORMAT_XRGB32, cacheEntry->scanline,
			0, 0, cacheEntry->width, cacheEntry->height, surface->data,
			PIXEL_FORMAT_XRGB32, surface->scanline, rect->left, rect->top);

	context->SetCacheSlotData(context, surfaceToCache->cacheSlot, (void*) cacheEntry);

	return 1;
}

int xf_CacheToSurface(RdpgfxClientContext* context, RDPGFX_CACHE_TO_SURFACE_PDU* cacheToSurface)
{
	UINT16 index;
	RDPGFX_POINT16* destPt;
	xfGfxSurface* surface;
	xfGfxCacheEntry* cacheEntry;
	RECTANGLE_16 invalidRect;
	xfContext* xfc = (xfContext*) context->custom;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cacheToSurface->surfaceId);
	cacheEntry = (xfGfxCacheEntry*) context->GetCacheSlotData(context, cacheToSurface->cacheSlot);

	//printf("xf_CacheToSurface: cacheEntry: %d\n",
	//		cacheToSurface->cacheSlot);

	if (!surface || !cacheEntry)
		return -1;

	for (index = 0; index < cacheToSurface->destPtsCount; index++)
	{
		destPt = &cacheToSurface->destPts[index];

		freerdp_image_copy(surface->data, PIXEL_FORMAT_XRGB32, surface->scanline,
				destPt->x, destPt->y, cacheEntry->width, cacheEntry->height,
				cacheEntry->data, PIXEL_FORMAT_XRGB32, cacheEntry->scanline, 0, 0);

		invalidRect.left = destPt->x;
		invalidRect.top = destPt->y;
		invalidRect.right = destPt->x + cacheEntry->width - 1;
		invalidRect.bottom = destPt->y + cacheEntry->height - 1;

		region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);
	}

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_CacheImportReply(RdpgfxClientContext* context, RDPGFX_CACHE_IMPORT_REPLY_PDU* cacheImportReply)
{
	return 1;
}

int xf_EvictCacheEntry(RdpgfxClientContext* context, RDPGFX_EVICT_CACHE_ENTRY_PDU* evictCacheEntry)
{
	xfGfxCacheEntry* cacheEntry;

	//printf("xf_EvictCacheEntry\n");

	cacheEntry = (xfGfxCacheEntry*) context->GetCacheSlotData(context, evictCacheEntry->cacheSlot);

	if (cacheEntry)
	{
		free(cacheEntry->data);
		free(cacheEntry);
	}

	context->SetCacheSlotData(context, evictCacheEntry->cacheSlot, NULL);

	return 1;
}

int xf_MapSurfaceToOutput(RdpgfxClientContext* context, RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* surfaceToOutput)
{
	xfContext* xfc = (xfContext*) context->custom;

	xfc->outputSurfaceId = surfaceToOutput->surfaceId;

	return 1;
}

int xf_MapSurfaceToWindow(RdpgfxClientContext* context, RDPGFX_MAP_SURFACE_TO_WINDOW_PDU* surfaceToWindow)
{
	return 1;
}

void xf_graphics_pipeline_init(xfContext* xfc, RdpgfxClientContext* gfx)
{
	xfc->gfx = gfx;
	gfx->custom = (void*) xfc;

	gfx->ResetGraphics = xf_ResetGraphics;
	gfx->StartFrame = xf_StartFrame;
	gfx->EndFrame = xf_EndFrame;
	gfx->SurfaceCommand = xf_SurfaceCommand;
	gfx->DeleteEncodingContext = xf_DeleteEncodingContext;
	gfx->CreateSurface = xf_CreateSurface;
	gfx->DeleteSurface = xf_DeleteSurface;
	gfx->SolidFill = xf_SolidFill;
	gfx->SurfaceToSurface = xf_SurfaceToSurface;
	gfx->SurfaceToCache = xf_SurfaceToCache;
	gfx->CacheToSurface = xf_CacheToSurface;
	gfx->CacheImportReply = xf_CacheImportReply;
	gfx->EvictCacheEntry = xf_EvictCacheEntry;
	gfx->MapSurfaceToOutput = xf_MapSurfaceToOutput;
	gfx->MapSurfaceToWindow = xf_MapSurfaceToWindow;

	region16_init(&(xfc->invalidRegion));
}

void xf_graphics_pipeline_uninit(xfContext* xfc, RdpgfxClientContext* gfx)
{
	region16_uninit(&(xfc->invalidRegion));

	gfx->custom = NULL;
	xfc->gfx = NULL;
}
