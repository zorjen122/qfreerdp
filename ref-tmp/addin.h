/*
 * Minimal client channel handler declarations needed by the local cliprdr copy.
 *
 * The full upstream header is channels/client/addin.h in the FreeRDP source
 * tree. FreeRDP development packages do not install that private header, so
 * this demo keeps only the declarations implemented by qf_channel_client_handler.c.
 */
#pragma once

#include <freerdp/api.h>
#include <freerdp/freerdp.h>
#include <winpr/stream.h>
#include <winpr/wtypes.h>

typedef UINT (*MsgHandler)(LPVOID userdata, wStream* data);

WINPR_ATTR_NODISCARD
FREERDP_API void* channel_client_create_handler(rdpContext* ctx, LPVOID userdata,
                                                MsgHandler handler, const char* channel_name);

WINPR_ATTR_NODISCARD
FREERDP_LOCAL UINT channel_client_post_message(void* MsgsHandle, LPVOID pData, UINT32 dataLength,
                                               UINT32 totalLength, UINT32 dataFlags);

FREERDP_LOCAL UINT channel_client_quit_handler(void* MsgsHandle);
