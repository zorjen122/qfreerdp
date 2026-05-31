#pragma once

#include <QObject>
#include <QQuickPaintedItem>
#include <QImage>
#include <QMutex>
#include <QPainter>
#include <QtQml/qqmlregistration.h>
#include <qnamespace.h>

#include "freerdp/freerdp.h"

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
    }
    ~RdpViewItem() = default;

    void setFreeRDP_context(rdpContext* context) {
        m_rdpContext = context;
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

        printf("Mouse event sent, event: %s, x: %d, y: %d\n", get_mouse_flags_string(freerdp_mouse_event).c_str(), map_x, map_y);
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

    void keyPressEvent(QKeyEvent* event) override {

        
        event->accept();
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        
        event->accept();
    }

private:
    QImage m_frame;
    QMutex m_mutex; // Mutex for thread-safe access to m_frame
    rdpContext* m_rdpContext;
};
