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
#include "qf_log.h"
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

	qf::log::info("cliprdr/format-list", "server advertised {} format(s)", formatList->numFormats);
	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const char* name = formatList->formats[i].formatName ? formatList->formats[i].formatName : "";
		g_client->clipboard_format_from_remote_[formatList->formats[i].formatId] = name;
		qf::log::debug("cliprdr/format-list", "remote[{}] id={} name={}", i,
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
		qf::log::info("cliprdr/format-select", "request remote CF_UNICODETEXT");
		requestRemoteFormat(CF_UNICODETEXT, nullptr);
		return CHANNEL_RC_OK;
	}

	for (UINT32 i = 0; i < formatList->numFormats; ++i)
	{
		const CLIPRDR_FORMAT* format = &formatList->formats[i];
		const char* name = format->formatName;
		if (name && !strcmp(name, "PNG"))
		{
			qf::log::info("cliprdr/format-select", "request remote PNG id={}", format->formatId);
			requestRemoteFormat(format->formatId, name);
			return CHANNEL_RC_OK;
		}
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIBV5))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_DIBV5");
		requestRemoteFormat(CF_DIBV5, nullptr);
		return CHANNEL_RC_OK;
	}

	if (g_client->clipboard_format_from_remote_.contains(CF_DIB))
	{
		qf::log::info("cliprdr/format-select", "request remote CF_DIB");
		requestRemoteFormat(CF_DIB, nullptr);
		return CHANNEL_RC_OK;
	}

	qf::log::debug("cliprdr/format-select", "no supported remote clipboard format");
	return CHANNEL_RC_OK;
}

UINT qf_CliprdrServerFormatDataResponseCallBack(
    CliprdrClientContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
	if (!context || !formatDataResponse)
	{
		qf::log::warn("cliprdr/data-response", "invalid clipboard data response");
		return ERROR_INVALID_PARAMETER;
	}

	if (formatDataResponse->common.msgFlags != CB_RESPONSE_OK)
	{
		qf::log::warn("cliprdr/data-response", "remote request failed formatId={} name={}",
		             g_client->requested_remote_format_id_, g_client->requested_remote_format_name);
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

	qf::log::debug("cliprdr/data-response", "received {} byte(s)",
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
		qf::log::warn("cliprdr/data-request", "invalid clipboard data request");
		return ERROR_INVALID_PARAMETER;
	}

	const QMimeData* mimeData = QGuiApplication::clipboard()->mimeData();
	if (!mimeData)
	{
		qf::log::warn("cliprdr/data-request", "no local clipboard data available");
		return CHANNEL_RC_OK;
	}

	qf::log::info("cliprdr/data-request", "server requested formatId={}",
	              formatDataRequest->requestedFormatId);

	auto RemoteFormatDataResponse = [&](const BYTE* rawData, UINT32 dataLen) {
		CLIPRDR_FORMAT_DATA_RESPONSE req = WINPR_C_ARRAY_INIT;
		req.common.msgFlags = CB_RESPONSE_OK;
		req.common.dataLen = dataLen;
		req.requestedFormatData = rawData;

		context->ClientFormatDataResponse(context, &req);
		qf::log::debug("cliprdr/data-request", "respond formatId={} bytes={}",
		               formatDataRequest->requestedFormatId, dataLen);
	};

	auto RemoteFormatDataFail = [&]() {
		CLIPRDR_FORMAT_DATA_RESPONSE req = WINPR_C_ARRAY_INIT;
		req.common.msgFlags = CB_RESPONSE_FAIL;
		req.common.dataLen = 0;
		req.requestedFormatData = nullptr;

		context->ClientFormatDataResponse(context, &req);
		qf::log::warn("cliprdr/data-request", "unsupported formatId={}",
		             formatDataRequest->requestedFormatId);
	};

	if (mimeData->hasText() && formatDataRequest->requestedFormatId == CF_UNICODETEXT)
	{
		const char16_t* rawData = reinterpret_cast<const char16_t*>(mimeData->text().utf16());
		UINT32 dataLen = (std::char_traits<char16_t>::length(rawData) + 1) * sizeof(char16_t); // 16bit char
		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(rawData), dataLen);

		qf::log::debug("cliprdr/data-request", "sent text formatId={} chars={}",
		               formatDataRequest->requestedFormatId, mimeData->text().size());
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == CF_DIB)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			qf::log::warn("cliprdr/data-request", "no local clipboard image available");
			return CHANNEL_RC_OK;
		}

		QByteArray bmp;
		QBuffer buffer(&bmp);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "BMP"))
		{
			qf::log::warn("cliprdr/data-request", "failed to encode clipboard image as DIB");
			return CHANNEL_RC_OK;
		}

		QByteArray dib = bmp.mid(14);	// skip 14 bytes of BMP header

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(dib.constData()),
		                         static_cast<UINT32>(dib.size()));
		qf::log::debug("cliprdr/data-request", "sent DIB formatId={} bytes={}",
		               formatDataRequest->requestedFormatId, dib.size());
	}
	else if (mimeData->hasImage() && formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_PNG)
	{
		auto image = qvariant_cast<QImage>(mimeData->imageData());
		if (image.isNull())
		{
			qf::log::warn("cliprdr/data-request", "no local clipboard image available");
			return CHANNEL_RC_OK;
		}

		QByteArray pngData;
		QBuffer buffer(&pngData);
		buffer.open(QIODevice::WriteOnly);
		if (!image.save(&buffer, "PNG"))
		{
			qf::log::warn("cliprdr/data-request", "failed to encode clipboard image as PNG");
			return CHANNEL_RC_OK;
		}

		RemoteFormatDataResponse(reinterpret_cast<const BYTE*>(pngData.constData()),
		                         static_cast<UINT32>(pngData.size()));
		qf::log::debug("cliprdr/data-request", "sent PNG formatId={} bytes={}",
		               formatDataRequest->requestedFormatId, pngData.size());
	}
	else if (mimeData->hasUrls() && formatDataRequest->requestedFormatId == qf::CLIPBOARD_FORMAT_FILE)
	{
		const size_t fileCount = g_client->clipboard_info_files_.size();
		if (fileCount == 0)
		{
			qf::log::warn("cliprdr/file-list", "no local file list for FileGroupDescriptorW request");
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
			qf::log::warn("cliprdr/file-list", "failed to serialize file list error={}", error);
			free(serialize_data);
			RemoteFormatDataFail();
			return CHANNEL_RC_OK;
		}

		RemoteFormatDataResponse(serialize_data, serialize_data_size);
		qf::log::info("cliprdr/file-list", "sent FileGroupDescriptorW files={} bytes={}", fileCount,
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
    qf::log::info("cliprdr/monitor", "monitor ready");

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

UINT qf_ServerFileContentsRequest(CliprdrClientContext* context, 
                                  const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	if (request->listIndex >= g_client->clipboard_info_files_.size())	
	{
		qf::log::warn("cliprdr/file-contents", "invalid listIndex={}", request->listIndex);
		return CB_RESPONSE_FAIL;
	}

	const auto& file_info = g_client->clipboard_info_files_[request->listIndex];
	if (file_info.is_directory_)
	{
		qf::log::warn("cliprdr/file-contents", "directory paste is not supported name={}", file_info.display_name_.toStdString());
		return CB_RESPONSE_FAIL;
	}

	auto sendFileContentResponse = [] (CliprdrClientContext* context, UINT32 streamId, const BYTE* data, UINT64 dataLen)
	{
		CLIPRDR_FILE_CONTENTS_RESPONSE response = WINPR_C_ARRAY_INIT;
		response.common.msgFlags = CB_RESPONSE_OK;
		response.streamId = streamId; // 0 for file contents request
		response.cbRequested = dataLen;
		response.requestedData = data;

		context->ClientFileContentsResponse(context, &response);
	};

	if (request->dwFlags & FILECONTENTS_SIZE)
	{
		uint64_t *size_mem = (uint64_t*)malloc(sizeof(uint64_t));
		*size_mem = file_info.total_;
		sendFileContentResponse(context, request->streamId, (const BYTE*) size_mem, sizeof(uint64_t));
		return CHANNEL_RC_OK;
	}

	if (request->dwFlags & FILECONTENTS_RANGE)
	{
		uint64_t offset = (uint64_t(request->nPositionHigh) << 32) | request->nPositionLow;

		if (offset >= file_info.total_)
		{
			qf::log::warn("cliprdr/file-contents", "offset out of range offset={} size={}", offset, file_info.total_);
			return CB_RESPONSE_FAIL;
		}

		QFile file(file_info.local_path_);
		if (!file.open(QIODevice::ReadOnly))
		{
			qf::log::warn("cliprdr/file-contents", "failed to open {}", file_info.local_path_.toStdString());
			return CB_RESPONSE_FAIL;
		}

		if (!file.seek(offset))
		{
			qf::log::warn("cliprdr/file-contents", "failed to seek {} offset={}", file_info.local_path_.toStdString(), offset);
			return CB_RESPONSE_FAIL;
		}

		const uint64_t remaining = file.size() - offset;
		const uint64_t bytesToRead = std::min(remaining, uint64_t(request->cbRequested));

		QByteArray data(bytesToRead, Qt::Uninitialized);
		if (!file.read(data.data(), bytesToRead))
		{
			qf::log::warn("cliprdr/file-contents", "failed to read {}", file_info.local_path_.toStdString());
			return CB_RESPONSE_FAIL;
		}

		char* data_mem = (char*)malloc(bytesToRead + 1); // copy data to heap
		memset(data_mem, 0, bytesToRead + 1); // clear data in heap
		memcpy(data_mem, data.constData(), bytesToRead);
		sendFileContentResponse(context, request->streamId, (const BYTE*) data_mem, bytesToRead);

		qf::log::info("cliprdr/file-contents", "sent range name={} offset={} bytes={}",
		             file_info.display_name_.toStdString(), offset, bytesToRead);
		return CHANNEL_RC_OK;
	}

	return CHANNEL_RC_OK;
}


void qt_clipboard_channel_init(CliprdrClientContext* clipboard)
{
	if (!clipboard)
	{
		qf::log::error("cliprdr/init", "clipboard channel init failed: null context");
		return;
	}

	g_client->clipboard_system_ = ClipboardCreate();
	if (!g_client->clipboard_system_)
	{
		qf::log::error("cliprdr/init", "WinPR clipboard system init failed");
		return;
	}

	if (!g_client->cliprdr_file_context_)
		g_client->cliprdr_file_context_ = cliprdr_file_context_new(g_client.get());
	if (!g_client->cliprdr_file_context_)
	{
		qf::log::error("cliprdr/init", "file context allocation failed");
		return;
	}

    clipboard->MonitorReady = qf_CliprdrMonitorReadyCallback;

	clipboard->ServerFormatList = qf_CliprdrServerFormatListCallBack;
	clipboard->ServerFormatListResponse = qf_CliprdrServerFormatListResponseCallBack;

    clipboard->ServerFormatDataRequest = qf_CliprdrServerFormatDataRequestCallBack;
    clipboard->ServerFormatDataResponse = qf_CliprdrServerFormatDataResponseCallBack;

	if(!cliprdr_file_context_init(g_client->cliprdr_file_context_, clipboard))
	{
		qf::log::error("cliprdr/init", "file context init failed");
		return;
	}

	clipboard->ServerFileContentsRequest = qf_ServerFileContentsRequest;
}

void qf_channel_connected_callback(void* context, const ChannelConnectedEventArgs* event)
{
	qf::log::info("channel/connect", "connected name={} interface={}", event->name, fmt::ptr(event->pInterface));

	if (!strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME))
	{
		qf::log::info("cliprdr/init", "initializing clipboard channel");
		qt_clipboard_channel_init(static_cast<CliprdrClientContext*>(event->pInterface));
	}
}

void qf_channel_disconnected_callback(void* context, const ChannelDisconnectedEventArgs* event)
{
}

static void qf_print_static_channels(rdpSettings* settings)
{
	const UINT32 count = freerdp_settings_get_uint32(settings, FreeRDP_StaticChannelCount);
	qf::log::debug("channel/static", "count={}", count);

	for (UINT32 i = 0; i < count; ++i)
	{
		const ADDIN_ARGV* args = static_cast<const ADDIN_ARGV*>(
		    freerdp_settings_get_pointer_array(settings, FreeRDP_StaticChannelArray, i));

		if (args && (args->argc > 0))
			qf::log::debug("channel/static", "channel[{}]={}", i, args->argv[0]);
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
			qf::log::error("cliprdr/load", "failed to add static channel");
			return FALSE;
		}
	}

	ADDIN_ARGV* cliprdrArgs = freerdp_static_channel_collection_find(
	    settings, CLIPRDR_SVC_CHANNEL_NAME);
	if (!cliprdrArgs)
	{
		qf::log::error("cliprdr/load", "static channel args missing after add");
		return FALSE;
	}

	qf_print_static_channels(settings);

	const int rc = freerdp_channels_client_load_ex(instance->context->channels, settings,
	                                               cliprdr_VirtualChannelEntryEx, cliprdrArgs);
	if (rc != CHANNEL_RC_OK)
	{
		qf::log::error("cliprdr/load", "failed to load channel add-in rc={}", rc);
		return FALSE;
	}

	qf::log::info("cliprdr/load", "channel add-in loaded");
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
		qf::log::error("cliprdr/load", "failed to load clipboard channel add-in");
		return FALSE;
	}

	return TRUE;
}

// 1. 预连接回调函数，在这里配置所有连接参数
static BOOL my_pre_connect(freerdp* instance)
{
	rdpSettings* settings = instance->context->settings;

	qf::log::info("rdp/pre-connect", "configuring connection settings");

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

	qf::log::info("rdp/pre-connect", "configuration applied");
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
		qf::log::error("rdp/post-connect", "gdi_init failed");
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

	qf::log::info("rdp/session", "starting FreeRDP loop");

	// 2. 创建并初始化一个 FreeRDP 实例
	freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
	g_instance = freerdp_new();
	if (!g_instance)
	{
		qf::log::error("rdp/session", "failed to allocate FreeRDP instance");
		return;
	}

	// 3. 注册预连接回调
	g_instance->PreConnect = my_pre_connect;
	g_instance->PostConnect = my_post_connect;
	g_instance->LoadChannels = my_load_channels;

	// 4. 创建 RDP 上下文，它会根据 instance 自动分配 settings 内存
	if (!freerdp_context_new(g_instance))
	{
		qf::log::error("rdp/session", "failed to allocate RDP context");
		goto fail;
	}

	// 5. 尝试连接
	qf::log::info("rdp/connect", "attempting to connect");

	// 如果 127.0.0.1:3389 没有开启的 RDP 服务，这里会很快报错并返回 FALSE。
	// 如果你有实际的 RDP 服务器，可以在 my_pre_connect 函数中填入正确的 IP/账号。
	if (!freerdp_connect(g_instance))
	{
		qf::log::error("rdp/connect", "connection failed");
	}
	else
	{
		qf::log::info("rdp/connect", "connected successfully");

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

		qf::log::info("rdp/disconnect", "disconnecting");
		freerdp_disconnect(g_instance);
	}

fail:
	qf::log::info("rdp/session", "test finished");
	freerdp_context_free(g_instance);
	freerdp_free(g_instance);

	qf::log::info("rdp/session", "FreeRDP instance freed rc={}", rc);
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
	qf::log::init();
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
