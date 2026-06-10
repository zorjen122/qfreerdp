#include "freerdp/codec/region.h"
#include "freerdp/event.h"
#include "freerdp/gdi/gdi.h"
#include "freerdp/settings_keys.h"
#include <cwchar>
#include <freerdp/freerdp.h>
#include <freerdp/log.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <algorithm>
#include <memory>
#include <qevent.h>
#include <qguiapplication.h>
#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/file.h>
#include <unistd.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QClipboard>
#include <QBuffer>
#include <QMimeData>
#include <map>

#include "freerdp/types.h"
#include "freerdp/channels/cliprdr.h"
#include "freerdp/client/cliprdr.h"
#include <freerdp/client/cmdline.h>
#include <freerdp/addin.h>
#include <freerdp/client/channels.h>
#include <freerdp/channels/channels.h>

#include "qf_util.h"
#include "rdp-view-item.h"

#define TAG CLIENT_TAG("sample")

extern "C" BOOL VCAPITYPE cliprdr_VirtualChannelEntryEx(PCHANNEL_ENTRY_POINTS_EX pEntryPoints,
                                                        PVOID pInitHandle);

static std::atomic<bool> g_stopped = false;
static RdpViewItem* g_rdpViewItem = nullptr;
static std::unique_ptr<std::thread> g_freerdp_thread = nullptr;
static std::shared_ptr<qf::client_t> g_client = {};
static freerdp* g_instance = nullptr;
static CliprdrClientContext* g_clipboard_client_context = nullptr;

static RECTANGLE_16 scale_frame(rdpContext* context, const RECTANGLE_16* rect)
{
	uint32_t hw = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
	uint32_t hh = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
	uint32_t cw = g_client->view_width_;
	uint32_t ch = g_client->view_height_;
	RECTANGLE_16 rect16 = *rect; // copy rect to rect16

	if (cw == 0)
		cw = hw;
	if (ch == 0)
		ch = hh;

	if (cw != hw || hh != ch)
	{
		rect16.left = rect->left * cw / hw;
		rect16.right = rect->right * cw / hw;
		rect16.top = rect->top * ch / hh;
		rect16.bottom = rect->bottom * ch / hh;
	}

	return rect16;
}

static BOOL noop_begin_paint(rdpContext* context)
{
	WINPR_UNUSED(context);
	return TRUE;
}

static BOOL noop_end_paint(rdpContext* context)
{
	rdpGdi* gdi = context->gdi;

	HGDI_DC hdc = gdi->primary->hdc;
	HGDI_WND hwnd = hdc->hwnd;
	REGION16 region16;

	if (!hwnd || hwnd->invalid->null == TRUE)
		return TRUE;

	region16_init(&region16);

	HGDI_RGN cinvalid = hwnd->cinvalid;
	for (int i = 0; i < hwnd->ninvalid; ++i)
	{
		RECTANGLE_16 rect;
		rect.left = cinvalid[i].x;
		rect.top = cinvalid[i].y;
		rect.right = cinvalid[i].x + cinvalid[i].w;
		rect.bottom = cinvalid[i].y + hwnd->cinvalid[i].h;
		region16_union_rect(&region16, &region16, &rect);
	}

	const RECTANGLE_16* rect = nullptr;
	if (!region16_is_empty(&region16))
	{
		rect = region16_extents(static_cast<const REGION16*>(&region16));
	}

	if (rect)
	{
		// TODO待优化的
		RECTANGLE_16 update_rect = scale_frame(context, rect);
		//

		QImage image(gdi->primary_buffer, gdi->width, gdi->height, QImage::Format_RGB32);

		QImage tmp = image.copy();
		QMetaObject::invokeMethod(
		    g_rdpViewItem,
		    [tmp]() { g_rdpViewItem->updateFrame(tmp, QRect(0, 0, tmp.width(), tmp.height())); });
		#if 0
		printf("draw region(left, top, right, bottom): (%d, %d) - (%d, %d)\n", rect->left,
		       rect->top, rect->right, rect->bottom);
		#endif
	}
	region16_uninit(&region16);

	return TRUE;
}

static BOOL noop_desktop_resize(rdpContext* context)
{
	WINPR_UNUSED(context);
	return TRUE;
}

static BOOL noop_bitmap_update(rdpContext* context, const BITMAP_UPDATE* bitmap)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(bitmap);
	return TRUE;
}

static BOOL noop_palette_update(rdpContext* context, const PALETTE_UPDATE* palette)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(palette);
	return TRUE;
}

static BOOL noop_play_sound(rdpContext* context, const PLAY_SOUND_UPDATE* play_sound)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(play_sound);
	return TRUE;
}

static BOOL noop_keyboard_set_indicators(rdpContext* context, UINT16 led_flags)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(led_flags);
	return TRUE;
}

static BOOL noop_keyboard_set_ime_status(rdpContext* context, UINT16 imeId, UINT32 imeState,
                                         UINT32 imeConvMode)
{
	WINPR_UNUSED(context);
	WINPR_UNUSED(imeId);
	WINPR_UNUSED(imeState);
	WINPR_UNUSED(imeConvMode);
	return TRUE;
}

static void qf_copy_file_descriptor_name(FILEDESCRIPTORW* descriptor, const QString& displayName)
{
	if (!descriptor)
		return;

	const qsizetype maxChars = static_cast<qsizetype>(ARRAYSIZE(descriptor->cFileName) - 1);
	const QString clippedName = displayName.left(static_cast<int>(maxChars));
	const ushort* utf16 = clippedName.utf16();

	for (qsizetype i = 0; i < clippedName.size(); ++i)
		descriptor->cFileName[i] = static_cast<WCHAR>(utf16[i]);
	descriptor->cFileName[clippedName.size()] = 0;
}


UINT qf_CliprdrServerFormatListCallBack(CliprdrClientContext* context,
                                        const CLIPRDR_FORMAT_LIST* formatList)
{
	if (!g_client->clipboard_format_from_remote_.empty())
		g_client->clipboard_format_from_remote_.clear();

	printf("cliprdr server format list: numFormats=%" PRIu32 "\n", formatList->numFormats);
	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const char* name = formatList->formats[i].formatName ? formatList->formats[i].formatName : "";
		g_client->clipboard_format_from_remote_[formatList->formats[i].formatId] = name;
		printf("  remote format[%" PRIu32 "]: id=%" PRIu32 ", name=%s\n", i,
		       formatList->formats[i].formatId, name);
	}

	auto requestRemoteFormat = [&](UINT32 formatId, const char* formatName)
	{
		CLIPRDR_FORMAT_DATA_REQUEST req = WINPR_C_ARRAY_INIT;
		req.requestedFormatId = formatId;

		g_client->requested_remote_format_id_ = formatId;
		g_client->requested_remote_format_name = formatName ? formatName : "";

		context->ClientFormatDataRequest(context, &req);
	};

	if (g_client->clipboard_format_from_remote_.contains(CF_UNICODETEXT))
	{
		printf("Remote clipboard supports CF_UNICODETEXT format\n");
		requestRemoteFormat(CF_UNICODETEXT, nullptr);
		return CHANNEL_RC_OK;
	}

	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const CLIPRDR_FORMAT* format = &formatList->formats[i];
		const char* name = format->formatName;
		if (name && !strcmp(name, "PNG"))
		{
			printf("Remote clipboard supports PNG format, remote formatId=%" PRIu32 "\n",
			       format->formatId);
			requestRemoteFormat(format->formatId, name);
			return CHANNEL_RC_OK;
		}
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIBV5))
	{
		printf("Remote clipboard supports CF_DIBV5 format\n");
		requestRemoteFormat(CF_DIBV5, nullptr);
		return CHANNEL_RC_OK;
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIB))
	{
		printf("Remote clipboard supports CF_DIB format\n");
		requestRemoteFormat(CF_DIB, nullptr);
		return CHANNEL_RC_OK;
	}

	printf("not support format\n");
	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatDataResponseCallBack(
    CliprdrClientContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
	if (!context || !formatDataResponse)
	{
		printf("Invalid clipboard data response\n");
		return ERROR_INVALID_PARAMETER;
	}

	if (formatDataResponse->common.msgFlags != CB_RESPONSE_OK)
	{
		printf("Remote clipboard data request failed, formatId=%" PRIu32 ", name=%s\n",
		       g_client->requested_remote_format_id_, g_client->requested_remote_format_name.c_str());
		return CHANNEL_RC_OK;
	}

	QByteArray clipboardData(reinterpret_cast<const char*>(formatDataResponse->requestedFormatData),
	                         static_cast<qsizetype>(formatDataResponse->common.dataLen));
	const UINT32 requestedFormatId = g_client->requested_remote_format_id_;
	const QString requestedFormatName = QString::fromStdString(g_client->requested_remote_format_name);
	QMetaObject::invokeMethod(g_rdpViewItem, [clipboardData, requestedFormatId, requestedFormatName]() {
		g_rdpViewItem->updateClipboardDataFromRemote(clipboardData, requestedFormatId,
		                                             requestedFormatName);
	}, Qt::QueuedConnection);

	printf("Clipboard data response received, dataLen=%" PRIu32 "\n",
	       formatDataResponse->common.dataLen);
	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatListResponseCallBack(
    CliprdrClientContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatDataRequestCallBack(CliprdrClientContext* context,
                                               const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	if (!context || !formatDataRequest)
	{
		printf("Invalid clipboard data request\n");
		return ERROR_INVALID_PARAMETER;
	}

	const QMimeData* mimeData = QGuiApplication::clipboard()->mimeData();
	if (!mimeData)
	{
		printf("No clipboard data available\n");
		return CHANNEL_RC_OK;
	}

	printf("Server requested clipboard formatId=%" PRIu32 "\n",
	       formatDataRequest->requestedFormatId);

	auto RemoteFormatDataResponse = [&](const BYTE* rawData, UINT32 dataLen) {
		CLIPRDR_FORMAT_DATA_RESPONSE req = WINPR_C_ARRAY_INIT;
		req.common.msgFlags = CB_RESPONSE_OK;
		req.common.dataLen = dataLen;
		req.requestedFormatData = rawData;

		context->ClientFormatDataResponse(context, &req);
		printf("From server data request, formatId=%" PRIu32 ", dataLen=%" PRIu32 "\n",
		       formatDataRequest->requestedFormatId, dataLen);
	};

	auto RemoteFormatDataFail = [&]() {
		CLIPRDR_FORMAT_DATA_RESPONSE req = WINPR_C_ARRAY_INIT;
		req.common.msgFlags = CB_RESPONSE_FAIL;
		req.common.dataLen = 0;
		req.requestedFormatData = nullptr;

		context->ClientFormatDataResponse(context, &req);
		printf("Unsupported server clipboard format request, formatId=%" PRIu32 "\n",
		       formatDataRequest->requestedFormatId);
	};

	if (mimeData->hasText() && formatDataRequest->requestedFormatId == CF_UNICODETEXT)
	{
		const char16_t* rawData = reinterpret_cast<const char16_t*>(mimeData->text().utf16());
		UINT32 dataLen = (std::char_traits<char16_t>::length(rawData) + 1) * sizeof(char16_t); // 16bit char
		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(rawData), dataLen);

		printf("From server data request, formatId=%" PRIu32 ", senting clipboard data: %s\n",
			formatDataRequest->requestedFormatId, mimeData->text().toUtf8().constData());
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == CF_DIB)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			printf("No clipboard image available\n");
			return CHANNEL_RC_OK;
		}

		QByteArray bmp;
		QBuffer buffer(&bmp);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "BMP"))
		{
			printf("Failed to encode clipboard image as DIB\n");
			return CHANNEL_RC_OK;
		}

		QByteArray dib = bmp.mid(14);	// skip 14 bytes of BMP header

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(dib.constData()),
		                         static_cast<UINT32>(dib.size()));
		printf("From server data request, formatId=%" PRIu32 ", sent DIB image bytes=%lld\n",
		       formatDataRequest->requestedFormatId, static_cast<long long>(dib.size()));
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_PNG)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			printf("No clipboard image available\n");
			return CHANNEL_RC_OK;
		}

		QByteArray pngData;
		QBuffer buffer(&pngData);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "PNG"))
		{
			printf("Failed to encode clipboard image as PNG\n");
			return CHANNEL_RC_OK;
		}

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(pngData.constData()),
		                         static_cast<UINT32>(pngData.size()));
		printf("From server data request, formatId=%" PRIu32 ", sent PNG image bytes=%lld\n",
		       formatDataRequest->requestedFormatId, static_cast<long long>(pngData.size()));
	}
	else if (mimeData->hasUrls() && formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_FILE)
	{
		const size_t fileCount = g_client->clipboard_info_files_.size();
		if (fileCount == 0)
		{
			printf("No local file list available for FileGroupDescriptorW request\n");
			RemoteFormatDataFail();
			return CHANNEL_RC_OK;
		}

		std::vector<FILEDESCRIPTORW> fds(fileCount);
		for (size_t i = 0; i < fileCount; ++i)
		{
			const auto& file_info = g_client->clipboard_info_files_[i];
			const UINT64 fileSize = file_info.is_directory_ ? 0 : file_info.total_;
			fds[i].nFileSizeLow = static_cast<DWORD>(fileSize & 0xFFFFFFFFULL);
			fds[i].nFileSizeHigh = static_cast<DWORD>((fileSize >> 32) & 0xFFFFFFFFULL);
			fds[i].dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
			fds[i].dwFileAttributes = file_info.is_directory_ ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
			qf_copy_file_descriptor_name(&fds[i], file_info.display_name_);
		}

		UINT32 flags = CB_STREAM_FILECLIP_ENABLED;
		if (g_client->cliprdr_file_context_)
		{
			const UINT32 remoteFlags = cliprdr_file_context_remote_get_flags(g_client->cliprdr_file_context_);
			if (remoteFlags & CB_STREAM_FILECLIP_ENABLED)
				flags = remoteFlags;
		}

		BYTE* serialize_data = nullptr;
		UINT32 serialize_data_size = 0;
		const UINT error = cliprdr_serialize_file_list_ex(flags, fds.data(), static_cast<UINT32>(fileCount),
		                                                  &serialize_data, &serialize_data_size);
		if (error || !serialize_data || serialize_data_size == 0)
		{
			printf("Failed to serialize file list, error=%" PRIu32 "\n", error);
			free(serialize_data);
			RemoteFormatDataFail();
			return CHANNEL_RC_OK;
		}

		RemoteFormatDataResponse(serialize_data, serialize_data_size);
		printf("Sent FileGroupDescriptorW for %zu local file(s), bytes=%" PRIu32 "\n", fileCount,
		       serialize_data_size);
		free(serialize_data);
	}
	else
	{
		RemoteFormatDataFail();
	}

	return CHANNEL_RC_OK;
}
UINT qf_CliprdrMonitorReadyCallback(CliprdrClientContext* context, const CLIPRDR_MONITOR_READY* monitorReady)
{
    g_client->cliprdr_client_context_ = context;
    printf("cliprdr monitor ready\n");

	CLIPRDR_CAPABILITIES capabilities = WINPR_C_ARRAY_INIT;
	CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet = WINPR_C_ARRAY_INIT;
	capabilities.cCapabilitiesSets = 1;
	capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&generalCapabilitySet);
	generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCapabilitySet.capabilitySetLength = 12;
	generalCapabilitySet.version = CB_CAPS_VERSION_2;
	generalCapabilitySet.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS;

	UINT rc = context->ClientCapabilities(context, &capabilities);
	if (rc != CHANNEL_RC_OK)
		return rc;

	CLIPRDR_FORMAT_LIST formatList = WINPR_C_ARRAY_INIT;
	return context->ClientFormatList(context, &formatList);
}


void qt_clipboard_channel_init(CliprdrClientContext* clipboard)
{
	if (!clipboard)
	{
		printf("clipboard channel init failed, since cliboard context is NULL\n");
		return;
	}

	g_client->clipboard_system_ = ClipboardCreate();
	if (!g_client->clipboard_system_)
	{
		printf("clipboard system init failed\n");
		return;
	}

	if (!g_client->cliprdr_file_context_)
		g_client->cliprdr_file_context_ = cliprdr_file_context_new(g_client.get());
	if (!g_client->cliprdr_file_context_)
	{
		printf("cliprdr file context alloc failed\n");
		return;
	}

    clipboard->MonitorReady = qf_CliprdrMonitorReadyCallback;

	clipboard->ServerFormatList = qf_CliprdrServerFormatListCallBack;
	clipboard->ServerFormatListResponse = qf_CliprdrServerFormatListResponseCallBack;

    clipboard->ServerFormatDataRequest = qf_CliprdrServerFormatDataRequestCallBack;
    clipboard->ServerFormatDataResponse = qf_CliprdrServerFormatDataResponseCallBack;

	if(!cliprdr_file_context_init(g_client->cliprdr_file_context_, clipboard))
	{
		printf("cliprdr file context init failed\n");
		return;
	}
}

void qf_channel_connected_callback(void* context, const ChannelConnectedEventArgs* event)
{
	printf("channel connected: %s, interface=%p\n", event->name, event->pInterface);

	if (!strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME))
	{
		printf("Initializing clipboard channel...\n");
		qt_clipboard_channel_init(static_cast<CliprdrClientContext*>(event->pInterface));
	}
}

void qf_channel_disconnected_callback(void* context, const ChannelDisconnectedEventArgs* event)
{
}

static void qf_print_static_channels(rdpSettings* settings)
{
	const UINT32 count = freerdp_settings_get_uint32(settings, FreeRDP_StaticChannelCount);
	printf("StaticChannelCount=%" PRIu32 "\n", count);

	for (UINT32 i = 0; i < count; ++i)
	{
		const ADDIN_ARGV* args = static_cast<const ADDIN_ARGV*>(
		    freerdp_settings_get_pointer_array(settings, FreeRDP_StaticChannelArray, i));

		if (args && (args->argc > 0))
			printf("static channel[%" PRIu32 "]=%s\n", i, args->argv[0]);
	}
}

static BOOL qf_load_cliprdr_addin(freerdp* instance)
{
	if (!instance || !instance->context || !instance->context->channels || !instance->context->settings)
		return FALSE;

	rdpSettings* settings = instance->context->settings;
	const char* const args[] = { CLIPRDR_SVC_CHANNEL_NAME };

	if (!freerdp_static_channel_collection_find(settings, CLIPRDR_SVC_CHANNEL_NAME))
	{
		if (!freerdp_client_add_static_channel(settings, ARRAYSIZE(args), args))
		{
			printf("[ERROR] Failed to add cliprdr static channel.\n");
			return FALSE;
		}
	}

	ADDIN_ARGV* cliprdrArgs = freerdp_static_channel_collection_find(
	    settings, CLIPRDR_SVC_CHANNEL_NAME);
	if (!cliprdrArgs)
	{
		printf("[ERROR] cliprdr static channel args were not found after add.\n");
		return FALSE;
	}

	qf_print_static_channels(settings);

	const int rc = freerdp_channels_client_load_ex(instance->context->channels, settings,
	                                               cliprdr_VirtualChannelEntryEx, cliprdrArgs);
	if (rc != CHANNEL_RC_OK)
	{
		printf("[ERROR] Failed to load cliprdr channel add-in, rc=%d.\n", rc);
		return FALSE;
	}

	printf("[INFO] cliprdr channel add-in loaded.\n");
	return TRUE;
}

static BOOL my_load_channels(freerdp* instance)
{
/*
[example]:
	PreConnect
	设置 ServerHostname / DesktopWidth / RedirectClipboard 等普通 settings

	utils_reload_channels
	重建 context->channels
	调用 instance->LoadChannels

	LoadChannels
	freerdp_client_add_static_channel(settings, "cliprdr")
		-> 把 cliprdr 加到 settings 的 StaticChannelArray
	freerdp_channels_client_load_ex(..., cliprdr_VirtualChannelEntryEx, ...)
		-> 调用 cliprdr_VirtualChannelEntryEx
		-> cliprdr 内部调用 VirtualChannelInitEx
		-> 把 channelDef.name = "cliprdr" 注册进 channel manager

	后续连接完成
	freerdp_channels_post_connect
		-> 发 CHANNEL_EVENT_CONNECTED 给 cliprdr
		-> cliprdr 调 VirtualChannelOpenEx 打开通道
		-> PubSub_OnChannelConnected
		-> 你的 qf_channel_connected_callback
*/
	if (!qf_load_cliprdr_addin(instance))
	{
		printf("[ERROR] Failed to load clipboard channel add-in.\n");
		return FALSE;
	}

	return TRUE;
}

// 1. 预连接回调函数，在这里配置所有连接参数
static BOOL my_pre_connect(freerdp* instance)
{
	rdpSettings* settings = instance->context->settings;

	printf("[INFO] Configuring connection settings...\n");

	// 【请根据实际情况修改以下参数】
	// 目标主机的 IP 和端口
	freerdp_settings_set_string(settings, FreeRDP_ServerHostname, "192.168.137.11");
	freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, 3389);

	// 登录凭证
	freerdp_settings_set_string(settings, FreeRDP_Username, "demo");
	freerdp_settings_set_string(settings, FreeRDP_Password, "rootroot1.");

	// 虚拟分辨率（即使无头也需要告诉服务端你请求的分辨率）
	freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, g_client->view_width_);
	freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, g_client->view_height_);
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

	// 跳过证书强校验（测试自签名证书时非常实用）
	freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

	// 允许本地图形解码。
	freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, FALSE);

	freerdp_settings_set_uint32(settings, FreeRDP_ProxyType, PROXY_TYPE_IGNORE);

	freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, FALSE);

	freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);

	PubSub_SubscribeChannelConnected(instance->context->pubSub, qf_channel_connected_callback);
	PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
	                                    qf_channel_disconnected_callback);

	printf("[INFO] Pre-connect configurations set.\n");
	return TRUE;
}

static BOOL my_post_connect(freerdp* instance)
{
	rdpUpdate* update = instance->context->update;

	/*
	 * Without these callbacks, the first bitmap/palette update can be treated
	 * as unhandled and the main loop exits immediately. For a headless test
	 * client we intentionally consume them without rendering.
	 */
	update->BeginPaint = noop_begin_paint;
	update->EndPaint = noop_end_paint;
	update->DesktopResize = noop_desktop_resize;
	// update->BitmapUpdate = noop_bitmap_update;
	update->Palette = noop_palette_update;
	update->PlaySound = noop_play_sound;
	update->SetKeyboardIndicators = noop_keyboard_set_indicators;
	update->SetKeyboardImeStatus = noop_keyboard_set_ime_status;

	if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
	{
		printf("gdi init format is failed");
		return FALSE;
	}

	g_rdpViewItem->setFreeRDP_context(instance->context);

	return TRUE;
}

void rdp_loop_thread(int argc, char* argv[])
{
	DWORD rc = 1;
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);

	printf("--- Minimal FreeRDP 3.x Headless Client Test ---\n");

	// 2. 创建并初始化一个 FreeRDP 实例
	freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
	g_instance = freerdp_new();
	if (!g_instance)
	{
		fprintf(stderr, "[ERROR] Failed to allocate FreeRDP instance.\n");
		return;
	}

	// 3. 注册预连接回调
	g_instance->PreConnect = my_pre_connect;
	g_instance->PostConnect = my_post_connect;
	g_instance->LoadChannels = my_load_channels;

	// 4. 创建 RDP 上下文，它会根据 instance 自动分配 settings 内存
	if (!freerdp_context_new(g_instance))
	{
		fprintf(stderr, "[ERROR] Failed to allocate RDP context.\n");
		goto fail;
	}

	// 5. 尝试连接
	printf("[INFO] Attempting to connect...\n");

	// 如果 127.0.0.1:3389 没有开启的 RDP 服务，这里会很快报错并返回 FALSE。
	// 如果你有实际的 RDP 服务器，可以在 my_pre_connect 函数中填入正确的 IP/账号。
	if (!freerdp_connect(g_instance))
	{
		fprintf(stderr, "[ERROR] Connection failed (this is normal if no RDP server is running on "
		                "target IP).\n");
	}
	else
	{
		printf("[SUCCESS] Connected successfully!\n");

		// 6. 主事件轮询，用来保持 TCP 链接畅通并接收协议数据
		DWORD nCount = 0;
		DWORD status = 0;
		HANDLE handles[MAXIMUM_WAIT_OBJECTS] = WINPR_C_ARRAY_INIT;
		while (!freerdp_shall_disconnect_context(g_instance->context) && !g_stopped)
		{
			nCount = freerdp_get_event_handles(g_instance->context, handles, ARRAYSIZE(handles));

			if (nCount == 0)
			{
				WLog_ERR(TAG, "freerdp_get_event_handles failed");
				break;
			}

			status = WaitForMultipleObjects(nCount, handles, FALSE, INFINITE);

			if (status == WAIT_FAILED)
			{
				WLog_ERR(TAG, "WaitForMultipleObjects failed with %" PRIu32 "", status);
				break;
			}

			if (!freerdp_check_event_handles(g_instance->context))
			{
				if (freerdp_get_last_error(g_instance->context) == FREERDP_ERROR_SUCCESS)
					WLog_ERR(TAG, "Failed to check FreeRDP event handles");

				break;
			}
		}

		rc = freerdp_get_last_error(g_instance->context);

		printf("[INFO] Disconnecting...\n");
		freerdp_disconnect(g_instance);
	}

fail:
	printf("--- Test finished ---\n");
	freerdp_context_free(g_instance);
	freerdp_free(g_instance);

	printf("[INFO] FreeRDP instance freed, rc = %d\n", rc);
}

void stop()
{
	g_stopped = true; // 设置停止标志
	if (g_freerdp_thread)
	{
		g_freerdp_thread->join(); // 等待子线程结束
		g_freerdp_thread.reset(); // 清理线程指针
	}
}

int main(int argc, char* argv[])
{
	QGuiApplication app(argc, argv);
	QQmlApplicationEngine engine;

	// Qt 6 会将打包的 QML 映射到 qrc:/URI名/文件名
	const QUrl url(QStringLiteral("qrc:/MyTestApp/main.qml"));

	QObject::connect(
	    &engine, &QQmlApplicationEngine::objectCreated, &app,
	    [url](QObject* obj, const QUrl& objUrl)
	    {
		    if (!obj && url == objUrl)
		    {
			    QCoreApplication::exit(-1);
		    }
	    },
	    Qt::QueuedConnection);

	engine.load(url);

	if (engine.rootObjects().isEmpty())
	{
		qWarning("Failed to load QML file.");
		stop();
		return -1;
	}

	QObject* root = engine.rootObjects().first();
	g_rdpViewItem = root->findChild<RdpViewItem*>("rdpViewItem");
	if (!g_rdpViewItem)
	{
		qWarning("Failed to find RdpViewItem in QML.");
		stop();
		return -1;
	}

	g_client = std::make_shared<qf::client_t>();
	g_rdpViewItem->set_qfclient_context(g_client);

	g_freerdp_thread = std::make_unique<std::thread>(rdp_loop_thread, argc, argv);

	int rt = app.exec();

	stop();

	return rt;
}
