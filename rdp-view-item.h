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
#include <QtEndian>
#include <QFileInfo>
#include <QDir>
#include <winpr/user.h>

#include <algorithm>

#include "freerdp/freerdp.h"
#include "freerdp/client/cliprdr.h"
#include "freerdp/input.h"

#include "qf_util.h"
#include "qf_log.h"

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

    void set_qfclient_context(std::shared_ptr<qf::client_t> context) {
        m_qfClientContext = context;
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
            qf::log::warn("input/mouse", "failed to send mouse event flags={} x={} y={}",
                          freerdp_mouse_event, map_x, map_y);
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
                    qf::log::warn("input/key", "failed to send unicode text={}", event->text().toStdString());
                    return;
                }
            }
            qf::log::debug("input/key", "unknown Qt key={} text='{}'", event->key(), event->text().toStdString());
            return;
        }

        qf::log::debug("input/key", "qtKey={} state={} rdpScancode=0x{:04x}", event->key(),
                       down ? "down" : "up", freerdp_key_code);
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


    static QImage imageFromDib(const QByteArray& dib) {
        if (dib.size() < 40)
            return {};

        const uchar* bytes = reinterpret_cast<const uchar*>(dib.constData());
        const quint32 headerSize = qFromLittleEndian<quint32>(bytes);
        if (headerSize < 12 || static_cast<qsizetype>(headerSize) > dib.size())
            return {};

        quint16 bitCount = 0;
        quint32 compression = 0;
        quint32 colorUsed = 0;
        if (headerSize >= 40 && dib.size() >= 40)
        {
            bitCount = qFromLittleEndian<quint16>(bytes + 14);
            compression = qFromLittleEndian<quint32>(bytes + 16);
            colorUsed = qFromLittleEndian<quint32>(bytes + 32);
        }

        quint32 colorTableBytes = 0;
        if (colorUsed > 0)
            colorTableBytes = colorUsed * 4;
        else if (bitCount > 0 && bitCount <= 8)
            colorTableBytes = (1u << bitCount) * 4;

        const quint32 bitfieldsBytes = (headerSize == 40 && compression == 3) ? 12 : 0;
        const quint32 pixelOffset = 14 + headerSize + bitfieldsBytes + colorTableBytes;
        const quint32 fileSize = 14 + static_cast<quint32>(dib.size());

        QByteArray bmp;
        bmp.reserve(static_cast<qsizetype>(fileSize));
        bmp.append('B');
        bmp.append('M');

        auto appendLe16 = [&bmp](quint16 value) {
            char buffer[2];
            qToLittleEndian(value, reinterpret_cast<uchar*>(buffer));
            bmp.append(buffer, sizeof(buffer));
        };
        auto appendLe32 = [&bmp](quint32 value) {
            char buffer[4];
            qToLittleEndian(value, reinterpret_cast<uchar*>(buffer));
            bmp.append(buffer, sizeof(buffer));
        };

        appendLe32(fileSize);
        appendLe16(0);
        appendLe16(0);
        appendLe32(pixelOffset);
        bmp.append(dib);

        return QImage::fromData(bmp, "BMP");
    }

    void updateClipboardFilesFromRemote(const std::vector<QString>& paths)
    {
        if (!m_qfClientContext || !m_qfClientContext->cliprdr_client_context_)
        {
            qf::log::debug("clipboard/remote", "no clipboard client context");
            return;
        }

        if (paths.empty())
        {
            qf::log::warn("clipboard/remote", "files path is empty");
            return;
        }

        QList<QUrl> urls;
        for (const auto& path : paths)
        {
            if (QFileInfo::exists(path))
                urls.append(QUrl::fromLocalFile(path));
        }

        if (urls.empty())
        {
            qf::log::warn("clipboard/remote", "no downloaded files exist");
            return;
        }

        m_clipboardDataFromRemote = true;
        auto* data = new QMimeData();
        data->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(data);
        m_clipboardDataFromRemote = false;

        qf::log::info("clipboard/remote", "updated local clipboard with remote files count={}",
                      urls.size());
    }

    void updateClipboardDataFromRemote(const QByteArray& data, uint32_t formatId,
                                       const QString& formatName) {
        if (!m_qfClientContext || !m_qfClientContext->cliprdr_client_context_)
        {
            qf::log::debug("clipboard/remote", "no clipboard client context");
            return;
        }

        m_clipboardDataFromRemote = true;

        if (formatName == QStringLiteral("PNG"))
        {
            QImage image = QImage::fromData(data, "PNG");
            if (!image.isNull())
            {
                QGuiApplication::clipboard()->setImage(image);
                qf::log::info("clipboard/remote", "updated local clipboard with remote PNG image");
            }
            else
                qf::log::warn("clipboard/remote", "failed to decode remote PNG bytes={}", data.size());
        }
        else if (formatId == CF_DIB || formatId == CF_DIBV5)
        {
            QImage image = imageFromDib(data);
            if (!image.isNull())
            {
                QGuiApplication::clipboard()->setImage(image);
                qf::log::info("clipboard/remote", "updated local clipboard with remote DIB image");
            }
            else
                qf::log::warn("clipboard/remote", "failed to decode remote DIB bytes={}", data.size());
        }
        else if(formatId == CF_UNICODETEXT)
        {
            qsizetype charCount = data.size() / static_cast<qsizetype>(sizeof(char16_t));
            const char16_t* textData = reinterpret_cast<const char16_t*>(data.constData());
            if (charCount > 0 && textData[charCount - 1] == u'\0')
                --charCount;

            QString text = QString::fromUtf16(textData, charCount);
            QGuiApplication::clipboard()->setText(text);
            qf::log::info("clipboard/remote", "updated local clipboard with remote text chars={}", text.size());
        }
        m_clipboardDataFromRemote = false;
    }

    QString clipboardDisplayName(const QFileInfo& root, const QFileInfo& fileInfo) const
    {
        QString displayName = root.fileName();
        if (root.absoluteFilePath() != fileInfo.absoluteFilePath())
        {
            QDir rootDir(root.absoluteFilePath());
            displayName += "/" + rootDir.relativeFilePath(fileInfo.absoluteFilePath());
        }
        displayName.replace("/", "\\");
        return displayName;
    }

    void appendClipboardInfoFile(const QFileInfo& root, const QFileInfo& fileInfo)
    {
        qf::clipboard_info_file_t clipboard_file_info;
        clipboard_file_info.display_name_ = clipboardDisplayName(root, fileInfo);
        clipboard_file_info.local_path_ = fileInfo.absoluteFilePath();
        clipboard_file_info.total_ = fileInfo.size();
        clipboard_file_info.is_directory_ = fileInfo.isDir();

        auto iter = std::find_if(m_qfClientContext->clipboard_info_files_.begin(), 
                    m_qfClientContext->clipboard_info_files_.end(),
                    [&](const qf::clipboard_info_file_t& file) {
                        return file.local_path_ == fileInfo.absoluteFilePath();
                    });

        bool contained = iter == m_qfClientContext->clipboard_info_files_.end();
        if (contained)
            m_qfClientContext->clipboard_info_files_.push_back(clipboard_file_info);

        if (!fileInfo.isDir())
            return;

        QDir dir(fileInfo.absoluteFilePath());
        for (const QString& file : dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
            appendClipboardInfoFile(root, QFileInfo(dir.filePath(file)));
    }

    // plugin for clipboard
    void dataChangedCallback() {
        if (!m_qfClientContext)
        {
            qf::log::debug("clipboard/local", "no qfreerdp client context");
            return;
        }

        auto& clipboardContext = m_qfClientContext->cliprdr_client_context_;
        if(!clipboardContext)
        {
            qf::log::debug("clipboard/remote", "no clipboard client context");
            return;
        }

        if (m_clipboardDataFromRemote)
        {
            qf::log::debug("clipboard/local", "ignore dataChanged from remote update");
            return;
        }

        auto RemoteClipboardFormatList = [&, this](uint32_t formatId, const char* formatName) {

            CLIPRDR_FORMAT_LIST format_list = {};

            CLIPRDR_FORMAT format = {};
            format.formatId = formatId;
            format.formatName = const_cast<char*>(formatName);

            format_list.numFormats = 1;
            format_list.formats = &format; // Clipboard data is in Unicode format

            clipboardContext->ClientFormatList(clipboardContext, &format_list);
            qf::log::info("clipboard/local-format", "advertise formatId={} name={}", formatId,
                          formatName ? formatName : "");

        };

        QClipboard* clipboard = QGuiApplication::clipboard();
        const QMimeData* mimeData = clipboard->mimeData();

        if (mimeData->hasUrls())
        {
            QByteArray uriList;
            m_qfClientContext->clipboard_info_files_.clear(); // Clear previous clipboard info files
            for (const QUrl& url : mimeData->urls())
            {
                if (url.isLocalFile())
                {
                    QFileInfo file(url.toLocalFile());

                    if (!file.isDir() && !file.isFile())
                    {
                        qf::log::warn("clipboard/local-file", 
                            "file type is not supported: {}", file.suffix().toStdString());
                        continue;
                    }

                    appendClipboardInfoFile(file, file);
                    uriList.append(url.toEncoded());
                    uriList.append('\n');
                }
            }

            if (m_qfClientContext->clipboard_info_files_.empty())
            {
                qf::log::warn("clipboard/local-file", "hasUrls but no local files were accepted");
                return;
            }

            if (m_qfClientContext->cliprdr_file_context_)
            {
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, TRUE);
                cliprdr_file_context_update_client_data(m_qfClientContext->cliprdr_file_context_,
                                                        uriList.constData(),
                                                        static_cast<size_t>(uriList.size()));
            }

            RemoteClipboardFormatList(qf::CLIPBOARD_FORMAT_FILE, qf::CLIPBOARD_FORMAT_FILE_NAME);
            qf::log::info("clipboard/local-file", "captured local file list files={} uriBytes={}",
                          m_qfClientContext->clipboard_info_files_.size(), uriList.size());
        }
        else if (mimeData->hasText()) {
            if (m_qfClientContext->cliprdr_file_context_)
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, FALSE);
            RemoteClipboardFormatList(CF_UNICODETEXT, nullptr);
        }
        else if (mimeData->hasImage()) {
            if (m_qfClientContext->cliprdr_file_context_)
                cliprdr_file_context_set_locally_available(m_qfClientContext->cliprdr_file_context_, FALSE);
            
            CLIPRDR_FORMAT_LIST format_list = {};

            CLIPRDR_FORMAT formats[2] = {};
            formats[0].formatId = qf::CLIPBOARD_FORMAT_PNG;
            formats[0].formatName = const_cast<char*>("PNG");
            formats[1].formatId = CF_DIB;
            formats[1].formatName = nullptr;

            format_list.numFormats = 2;
            format_list.formats = formats; // Clipboard data is in Unicode format

            clipboardContext->ClientFormatList(clipboardContext, &format_list);
            
            qf::log::info("clipboard/local-format", "advertise image formats [{}/{}], [{}/{}]",
                          formats[0].formatId, formats[0].formatName ? formats[0].formatName : "",
                          formats[1].formatId, formats[1].formatName ? formats[1].formatName : "");
        } 
    }

signals:
    void clipboardDataResponseFromRemote();

private:
    QImage m_frame;
    QMutex m_mutex; // Mutex for thread-safe access to m_frame
    rdpContext* m_rdpContext;
    std::shared_ptr<qf::client_t> m_qfClientContext; // Clipboard client context for clipboard operations
    bool m_clipboardDataFromRemote = false;
};
