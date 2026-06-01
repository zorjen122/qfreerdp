#include <freerdp/channels/channels.h>
#include <freerdp/channels/log.h>
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <freerdp/settings_keys.h>

#include <winpr/assert.h>
#include <winpr/collections.h>
#include <winpr/crt.h>
#include <winpr/synch.h>

#include "channels/client/addin.h"

#define TAG CHANNELS_TAG("qf.addin")

typedef struct
{
	wMessageQueue* queue;
	wStream* data_in;
	HANDLE thread;
	char* channel_name;
	rdpContext* ctx;
	LPVOID userdata;
	MsgHandler msg_handler;
} msg_proc_internals;

static DWORD WINAPI channel_client_thread_proc(LPVOID userdata)
{
	UINT error = CHANNEL_RC_OK;
	wStream* data = NULL;
	wMessage message = WINPR_C_ARRAY_INIT;
	msg_proc_internals* internals = userdata;

	WINPR_ASSERT(internals);

	while (1)
	{
		if (!MessageQueue_Wait(internals->queue))
		{
			WLog_ERR(TAG, "MessageQueue_Wait failed!");
			error = ERROR_INTERNAL_ERROR;
			break;
		}
		if (!MessageQueue_Peek(internals->queue, &message, TRUE))
		{
			WLog_ERR(TAG, "MessageQueue_Peek failed!");
			error = ERROR_INTERNAL_ERROR;
			break;
		}

		if (message.id == WMQ_QUIT)
			break;

		if (message.id == 0)
		{
			data = (wStream*)message.wParam;

			if ((error = internals->msg_handler(internals->userdata, data)))
			{
				WLog_ERR(TAG, "msg_handler failed with error %" PRIu32 "!", error);
				break;
			}
		}
	}

	if (error && internals->ctx)
		setChannelError(internals->ctx, error,
		                "%s_virtual_channel_client_thread reported an error",
		                internals->channel_name ? internals->channel_name : "unknown");

	ExitThread(error);
	return error;
}

static void free_msg(void* obj)
{
	wMessage* msg = (wMessage*)obj;

	if (msg && (msg->id == 0))
	{
		wStream* s = (wStream*)msg->wParam;
		Stream_Free(s, TRUE);
	}
}

static void channel_client_handler_free(msg_proc_internals* internals)
{
	if (!internals)
		return;

	if (internals->thread)
		(void)CloseHandle(internals->thread);
	MessageQueue_Free(internals->queue);
	Stream_Free(internals->data_in, TRUE);
	free(internals->channel_name);
	free(internals);
}

void* channel_client_create_handler(rdpContext* ctx, LPVOID userdata, MsgHandler msg_handler,
                                    const char* channel_name)
{
	msg_proc_internals* internals = calloc(1, sizeof(msg_proc_internals));
	if (!internals)
	{
		WLog_ERR(TAG, "calloc failed!");
		return NULL;
	}

	internals->msg_handler = msg_handler;
	internals->userdata = userdata;
	if (channel_name)
	{
		internals->channel_name = _strdup(channel_name);
		if (!internals->channel_name)
			goto fail;
	}

	WINPR_ASSERT(ctx);
	WINPR_ASSERT(ctx->settings);
	internals->ctx = ctx;

	if ((freerdp_settings_get_uint32(ctx->settings, FreeRDP_ThreadingFlags) &
	     THREADING_FLAGS_DISABLE_THREADS) == 0)
	{
		wObject obj = WINPR_C_ARRAY_INIT;
		obj.fnObjectFree = free_msg;
		internals->queue = MessageQueue_New(&obj);
		if (!internals->queue)
		{
			WLog_ERR(TAG, "MessageQueue_New failed!");
			goto fail;
		}

		internals->thread = CreateThread(NULL, 0, channel_client_thread_proc, internals, 0, NULL);
		if (!internals->thread)
		{
			WLog_ERR(TAG, "CreateThread failed!");
			goto fail;
		}
	}

	return internals;

fail:
	channel_client_handler_free(internals);
	return NULL;
}

UINT channel_client_post_message(void* MsgsHandle, LPVOID pData, UINT32 dataLength,
                                 UINT32 totalLength, UINT32 dataFlags)
{
	msg_proc_internals* internals = MsgsHandle;
	wStream* data_in = NULL;

	if (!internals)
		return CHANNEL_RC_OK;

	if ((dataFlags & CHANNEL_FLAG_SUSPEND) || (dataFlags & CHANNEL_FLAG_RESUME))
		return CHANNEL_RC_OK;

	if (dataFlags & CHANNEL_FLAG_FIRST)
	{
		if (internals->data_in)
		{
			if (!Stream_EnsureCapacity(internals->data_in, totalLength))
				return CHANNEL_RC_NO_MEMORY;
		}
		else
			internals->data_in = Stream_New(NULL, totalLength);
	}

	if (!(data_in = internals->data_in))
	{
		WLog_ERR(TAG, "Stream_New failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	if (!Stream_EnsureRemainingCapacity(data_in, dataLength))
	{
		Stream_Free(internals->data_in, TRUE);
		internals->data_in = NULL;
		return CHANNEL_RC_NO_MEMORY;
	}

	Stream_Write(data_in, pData, dataLength);

	if (dataFlags & CHANNEL_FLAG_LAST)
	{
		if (Stream_Capacity(data_in) != Stream_GetPosition(data_in))
		{
			WLog_ERR(TAG, "%s_plugin_process_received: read error", internals->channel_name);
			return ERROR_INTERNAL_ERROR;
		}

		internals->data_in = NULL;
		Stream_SealLength(data_in);
		Stream_ResetPosition(data_in);

		if ((freerdp_settings_get_uint32(internals->ctx->settings, FreeRDP_ThreadingFlags) &
		     THREADING_FLAGS_DISABLE_THREADS) != 0)
		{
			const UINT error = internals->msg_handler(internals->userdata, data_in);
			if (error)
			{
				WLog_ERR(TAG, "msg_handler failed with error %" PRIu32 "!", error);
				return ERROR_INTERNAL_ERROR;
			}
		}
		else if (!MessageQueue_Post(internals->queue, NULL, 0, data_in, NULL))
		{
			WLog_ERR(TAG, "MessageQueue_Post failed!");
			return ERROR_INTERNAL_ERROR;
		}
	}

	return CHANNEL_RC_OK;
}

UINT channel_client_quit_handler(void* MsgsHandle)
{
	msg_proc_internals* internals = MsgsHandle;
	UINT rc = 0;

	if (!internals)
		return CHANNEL_RC_OK;

	WINPR_ASSERT(internals->ctx);
	WINPR_ASSERT(internals->ctx->settings);

	if ((freerdp_settings_get_uint32(internals->ctx->settings, FreeRDP_ThreadingFlags) &
	     THREADING_FLAGS_DISABLE_THREADS) == 0)
	{
		if (internals->queue && internals->thread)
		{
			if (MessageQueue_PostQuit(internals->queue, 0) &&
			    (WaitForSingleObject(internals->thread, INFINITE) == WAIT_FAILED))
			{
				rc = GetLastError();
				WLog_ERR(TAG, "WaitForSingleObject failed with error %" PRIu32 "", rc);
				return rc;
			}
		}
	}

	channel_client_handler_free(internals);
	return CHANNEL_RC_OK;
}
