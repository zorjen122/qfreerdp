#include "freerdp/codec/region.h"
#include "freerdp/gdi/gdi.h"
#include "freerdp/settings_keys.h"
#include <freerdp/freerdp.h>
#include <freerdp/log.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <memory>
#include <winpr/crt.h>
#include <winpr/synch.h>
#include <unistd.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>

#include "freerdp/types.h"
#include "rdp-view-item.h"


#define TAG CLIENT_TAG("sample")

struct client_t {
    uint32_t view_width = 1024;
    uint32_t view_height = 768;
};

static std::atomic<bool> g_stopped = false;
static RdpViewItem* g_rdpViewItem = nullptr;
static std::unique_ptr<std::thread> g_freerdp_thread = nullptr;
static client_t g_client;
static freerdp* g_instance = nullptr;

static RECTANGLE_16 scale_frame(rdpContext* context, const RECTANGLE_16* rect)
{
    uint32_t hw = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
    uint32_t hh = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
    uint32_t cw = g_client.view_width;
    uint32_t ch = g_client.view_height;
    RECTANGLE_16 rect16 = *rect; // copy rect to rect16

    if (cw == 0) cw = hw;
    if (ch == 0) ch = hh;

    if (cw != hw || hh != ch) {
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

    if(!hwnd || hwnd->invalid->null == TRUE)
        return TRUE;

    region16_init(&region16);

    HGDI_RGN cinvalid = hwnd->cinvalid;
    for (int i = 0; i < hwnd->ninvalid; ++i)
    {
        RECTANGLE_16 rect;
        rect.left = cinvalid[i].x;
        rect.top = cinvalid[i].y;
        rect.right = cinvalid[i].x + cinvalid[i].w;
        rect.bottom = cinvalid[i].y +  hwnd->cinvalid[i].h;
        region16_union_rect(&region16, &region16, &rect);
    }

    const RECTANGLE_16* rect = nullptr;
    if (!region16_is_empty(&region16))
    {
        rect = region16_extents(static_cast<const REGION16*> (&region16));
    }

    if(rect)
    {
        // TODO待优化的
        RECTANGLE_16 update_rect = scale_frame(context, rect);
        // 

        QImage image(gdi->primary_buffer, 
            gdi->width,
            gdi->height,
            QImage::Format_RGB32);
        
        QImage tmp = image.copy();
        QMetaObject::invokeMethod(g_rdpViewItem, [tmp]() {
            g_rdpViewItem->updateFrame(tmp, QRect(0, 0, tmp.width(), tmp.height()));
        });

        printf("draw region(left, top, right, bottom): (%d, %d) - (%d, %d)\n", rect->left, rect->top, rect->right, rect->bottom);
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

// 1. 预连接回调函数，在这里配置所有连接参数
static BOOL my_pre_connect(freerdp* instance) {
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
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, g_client.view_width);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, g_client.view_height);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

    // 跳过证书强校验（测试自签名证书时非常实用）
    freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

    // 无头客户端只解析/接收协议更新，不做本地图形解码。
    freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, FALSE);

    freerdp_settings_set_uint32(settings, FreeRDP_ProxyType, PROXY_TYPE_IGNORE);

    printf("[INFO] Pre-connect configurations set.\n");
    return TRUE;
}

static BOOL my_post_connect(freerdp* instance) {
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

void rdp(int argc, char* argv[]) {
    DWORD rc = 1;
    WINPR_UNUSED(argc);
    WINPR_UNUSED(argv);

    printf("--- Minimal FreeRDP 3.x Headless Client Test ---\n");

    // 2. 创建并初始化一个 FreeRDP 实例
    g_instance = freerdp_new();
    if (!g_instance) {
        fprintf(stderr, "[ERROR] Failed to allocate FreeRDP instance.\n");
        return;
    }

    // 3. 注册预连接回调
    g_instance->PreConnect = my_pre_connect;
    g_instance->PostConnect = my_post_connect;

    // 4. 创建 RDP 上下文，它会根据 instance 自动分配 settings 内存
    if (!freerdp_context_new(g_instance)) {
        fprintf(stderr, "[ERROR] Failed to allocate RDP context.\n");
        goto fail;
    }

    // 5. 尝试连接
    printf("[INFO] Attempting to connect...\n");
    
    // 如果 127.0.0.1:3389 没有开启的 RDP 服务，这里会很快报错并返回 FALSE。
    // 如果你有实际的 RDP 服务器，可以在 my_pre_connect 函数中填入正确的 IP/账号。
    if (!freerdp_connect(g_instance)) {
        fprintf(stderr, "[ERROR] Connection failed (this is normal if no RDP server is running on target IP).\n");
    } else {
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

void stop() {
    g_stopped = true; // 设置停止标志
    if (g_freerdp_thread) {
        g_freerdp_thread->join(); // 等待子线程结束
        g_freerdp_thread.reset(); // 清理线程指针
    }
}

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    // Qt 6 会将打包的 QML 映射到 qrc:/URI名/文件名
    const QUrl url(QStringLiteral("qrc:/MyTestApp/main.qml"));
    
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl) {
            QCoreApplication::exit(-1);
        }
    }, Qt::QueuedConnection);

    engine.load(url);

    if (engine.rootObjects().isEmpty()) {
        qWarning("Failed to load QML file.");
        stop();
        return -1;
    }

    g_freerdp_thread = std::make_unique<std::thread>(rdp, argc, argv);

    QObject* root = engine.rootObjects().first();
    g_rdpViewItem = root->findChild<RdpViewItem*>("rdpViewItem");
    if(!g_rdpViewItem)
    {
        qWarning("Failed to find RdpViewItem in QML.");
        stop();
        return -1;
    }

    int rt = app.exec();
    
    stop();

    return rt;   
}