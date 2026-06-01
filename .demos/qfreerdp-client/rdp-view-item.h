#pragma once

#include <QObject>
#include <QQuickPaintedItem>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QtQml/qqmlregistration.h>
#include <qeventloop.h>
#include <qnamespace.h>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QByteArray>

#include "freerdp/freerdp.h"
#include "freerdp/client/cliprdr.h"
#include "freerdp/input.h"

#include "qf_util.h"

class RdpViewItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT // Declare this class as a QML element
public:
    RdpViewItem(QQuickItem* parent = nullptr) : QQuickPaintedItem(parent) {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setFlag(QQuickItem::ItemIsFocusScope, true);
        setFocus(true);

        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &RdpViewItem::dataChangedCallback);
    }
    ~RdpViewItem() = default;

    void setFreeRDP_context(rdpContext* context) {
        m_rdpContext = context;
    }

    void set_clipboard_client_context(CliprdrClientContext* clipboard_client_context) {
        m_clipboardClientContext = clipboard_client_context;
    }

    void set_clipboard_data_memory(const std::shared_ptr<char[]>& data) {
        m_clipboard_ready_data_ = data;
    }

    void paint(QPainter* painter) override {
        // Implement your painting logic here
        if (!m_frame.isNull())
        {
            painter->drawImage(boundingRect(), m_frame);
        }
    }

    void updateFrame(const QImage& frame, const QRect& region) {
        
        m_frame = frame;
        update(region); // Trigger a repaint only for the specified region
    }

    std::string get_mouse_flags_string(UINT16 flags) {
        std::string buffer{};

        // Check Action State
        if (flags & PTR_FLAGS_MOVE) buffer += "MOVE, ";
        if (flags & PTR_FLAGS_DOWN) buffer += "DOWN, ";

        // Check Buttons
        if (flags & PTR_FLAGS_BUTTON1) buffer += "BUTTON1 (Left), ";
        if (flags & PTR_FLAGS_BUTTON2) buffer += "BUTTON2 (Right), ";
        if (flags & PTR_FLAGS_BUTTON3) buffer += "BUTTON3 (Middle), ";
        
        // Check Scroll Wheels
        if (flags & PTR_FLAGS_WHEEL) buffer += "WHEEL, ";
        if (flags & PTR_FLAGS_HWHEEL) buffer += "HWHEEL, ";

        return buffer;
    }


    void mouseEventScaleSend(uint32_t mouse_x, uint32_t mouse_y, uint16_t freerdp_mouse_event) {
   
        if(!m_rdpContext)
            return;

        uint32_t host_w = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopWidth);
        uint32_t host_h = freerdp_settings_get_uint32(m_rdpContext->settings, FreeRDP_DesktopHeight);
    
        uint32_t map_x = mouse_x * host_w / width();
        uint32_t map_y = mouse_y * host_h / height();

        if (!freerdp_input_send_mouse_event(m_rdpContext->input, freerdp_mouse_event, map_x, map_y))
        {
            printf("Failed to send mouse event\n");
        }

        #if 0
        printf("Mouse event sent, event: %s, x: %d, y: %d\n", get_mouse_flags_string(freerdp_mouse_event).c_str(), map_x, map_y);
        #endif
    }

    void mousePressEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN : PTR_FLAGS_BUTTON2 | PTR_FLAGS_DOWN;
        mouseEventScaleSend(event->position().x(), event->position().y(), flags); // Left mouse button

        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        uint16_t flags = (event->button() == Qt::LeftButton) ? PTR_FLAGS_BUTTON1 : PTR_FLAGS_BUTTON2;
        mouseEventScaleSend(event->position().x(), event->position().y(), flags); // Left mouse button
        
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        mouseEventScaleSend(event->position().x(), event->position().y(), PTR_FLAGS_MOVE); // Left mouse button
        
        event->accept();
    }
    void wheelEvent(QWheelEvent* event) override {
        int delta = event->angleDelta().y();
        uint16_t flags = PTR_FLAGS_WHEEL;
        if (delta < 0)
        {
            flags |= PTR_FLAGS_WHEEL_NEGATIVE;
            delta = -delta; // Convert to positive for freerdp_input_send_mouse_event
        }

        flags |= static_cast<uint16_t>(delta) & WheelRotationMask;
        mouseEventScaleSend(event->position().x(), event->position().y(), flags); // Left mouse button
        
        event->accept();
    }

    void keyboardUnicodeEventSend(QKeyEvent* event, bool down)
    {
        if (!m_rdpContext)
            return;

        UINT32 freerdp_key_code = qf::to_freerdp_key_code(event);
        if (freerdp_key_code == RDP_SCANCODE_UNKNOWN) {
            
            uint16_t flags = down ? 0 : KBD_FLAGS_RELEASE;
            if (!event->text().isEmpty())
            {
                if(!freerdp_input_send_unicode_keyboard_event(m_rdpContext->input, flags, event->text().unicode()->unicode()))
                {
                    printf("Failed to send unicode keyboard event, %s\n", event->text().toUtf8().constData());
                    return;
                }
            }
            printf("Unknown Qt key %d, text='%s'\n", event->key(), event->text().toUtf8().constData());
            return;
        }

        printf("Key %d %s, rdp_scancode=0x%04x\n", event->key(), down ? "down" : "up",
               freerdp_key_code);
        freerdp_input_send_keyboard_event_ex(m_rdpContext->input, down,
                                             down && event->isAutoRepeat(), freerdp_key_code);
    }

    void keyPressEvent(QKeyEvent* event) override {
        keyboardUnicodeEventSend(event, true);
        
        event->accept();
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        keyboardUnicodeEventSend(event, false);
        
        event->accept();
    }


    void updateClipboardDataFromRemote(const QByteArray& data) {
        if (!m_clipboardClientContext)
        {
            printf("No clipboard client context\n");
            return;
        }

        qsizetype charCount = data.size() / static_cast<qsizetype>(sizeof(char16_t));
        const char16_t* textData = reinterpret_cast<const char16_t*>(data.constData());
        if (charCount > 0 && textData[charCount - 1] == u'\0')
            --charCount;

        QString text = QString::fromUtf16(textData, charCount);
        m_clipboardDataFromRemote = true;
        QGuiApplication::clipboard()->setText(text);
        m_clipboardDataFromRemote = false;
        printf("Updated clipboard with remote text data: %s\n", text.toUtf8().constData());
    }

    // plugin for clipboard
    void dataChangedCallback() {
        if(!m_clipboardClientContext)
        {
            printf("No clipboard client context\n");
            return;
        }

        if (m_clipboardDataFromRemote)
        {
            printf("Clipboard data from remote\n");
            return;
        }

        QClipboard* clipboard = QGuiApplication::clipboard();
        const QMimeData* mimeData = clipboard->mimeData();

        if (mimeData->hasText()) {
            CLIPRDR_FORMAT_LIST format_list = {};

            CLIPRDR_FORMAT format = {};
            format.formatId = CF_UNICODETEXT;

            format_list.numFormats = 1;
            format_list.formats = &format; // Clipboard data is in Unicode format

            m_clipboardClientContext->ClientFormatList(m_clipboardClientContext, &format_list);
        }
    }

signals:
    void clipboardDataResponseFromRemote();

private:
    QImage m_frame;
    QMutex m_mutex; // Mutex for thread-safe access to m_frame
    rdpContext* m_rdpContext;
    CliprdrClientContext* m_clipboardClientContext; // Clipboard client context for clipboard operations
    bool m_clipboardDataFromRemote = false;
    std::shared_ptr<char[]> m_clipboard_ready_data_; // Clipboard data memory
};
