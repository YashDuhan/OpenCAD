/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiattachments.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonObject>
#include <QMimeData>
#include <QPainter>
#include <QUrl>
#include <QUuid>

#ifdef LC_AI_PDF
#include <QPdfDocument>
#include <QTemporaryFile>
#endif

#include "lc_aiinferencemanager.h"

namespace {
    constexpr int PREVIEW_EDGE = 480;        // the strip and the conversation show small pictures only
    constexpr int PDF_PAGE_EDGE = 1400;      // pages sent as images: readable drawings and text
    constexpr int PDF_PREVIEW_HEIGHT = 200;
    const QStringList IMAGE_SUFFIXES = {"png", "jpg", "jpeg", "webp", "gif", "bmp"};

    QString newId() {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    QString base64(const QByteArray& data) {
        return QString::fromLatin1(data.toBase64());
    }

    QByteArray encode(const QImage& image, const char* format, const int quality = -1) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, format, quality);
        return bytes;
    }

    /** JPEG has no transparency: transparent parts become white, as they look in the chat. */
    QImage flattened(const QImage& image) {
        if (!image.hasAlphaChannel()) {
            return image.convertToFormat(QImage::Format_RGB32);
        }
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        painter.end();
        return flat;
    }

    QImage previewOf(const QImage& image) {
        return image.width() > PREVIEW_EDGE || image.height() > PREVIEW_EDGE
                   ? image.scaled(PREVIEW_EDGE, PREVIEW_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                   : image;
    }

    /**
     * The image as it is sent: its own bytes when they are a PNG, JPEG or WebP that fits; otherwise scaled down
     * to MAX_IMAGE_EDGE and saved as PNG (sharp lines, transparency) or, when that is too big, JPEG.
     */
    bool encodeImage(QImage image, const QByteArray& original, const QByteArray& format, LC_AIAttachment& out, QString& error) {
        using namespace LC_AIAttachments;
        if (image.isNull()) {
            error = QObject::tr("%1 is not an image OpenCAD can read.").arg(out.name);
            return false;
        }
        const QSize originalSize = image.size();
        if (image.width() > MAX_IMAGE_EDGE || image.height() > MAX_IMAGE_EDGE) {
            image = image.scaled(MAX_IMAGE_EDGE, MAX_IMAGE_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        const bool sendable = format == "png" || format == "jpeg" || format == "jpg" || format == "webp";
        QByteArray bytes;
        QString mime;
        if (image.size() == originalSize && sendable && !original.isEmpty() && original.size() <= MAX_IMAGE_BYTES) {
            bytes = original;
            mime = format == "png" ? "image/png" : format == "webp" ? "image/webp" : "image/jpeg";
        }
        else {
            if (format != "jpeg" && format != "jpg") { // photos stay JPEG; drawings and screenshots try PNG first
                bytes = encode(image, "PNG");
                mime = "image/png";
            }
            // still too big: JPEG, losing quality and then size until it fits
            for (int step = 0; (bytes.isEmpty() || bytes.size() > MAX_IMAGE_BYTES) && step < 9; ++step) {
                if (step == 0) {
                    image = flattened(image);
                }
                else if (step % 3 == 0) {
                    image = image.scaled(image.size() * 0.75, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
                bytes = encode(image, "JPEG", 90 - (step % 3) * 12);
                mime = "image/jpeg";
            }
        }
        const bool scaled = image.size() != originalSize;
        if (bytes.isEmpty() || bytes.size() > MAX_IMAGE_BYTES) {
            error = QObject::tr("%1 could not be made small enough to send.").arg(out.name);
            return false;
        }
        out.kind = LC_AIAttachment::Kind::Image;
        out.id = newId();
        out.mimeType = mime;
        out.data = bytes;
        out.size = image.size();
        out.preview = previewOf(image);
        if (scaled) {
            out.note = QObject::tr("scaled down from %1 × %2 px").arg(originalSize.width()).arg(originalSize.height());
        }
        return true;
    }

#ifdef LC_AI_PDF
    /**
     * Opens the PDF bytes with Qt Pdf (from a temporary file: loading from memory is asynchronous). When it cannot,
     * `status` says why; it stays None when the PDF itself was not the problem (no temporary file).
     */
    template <typename Use>
    bool withPdf(const QByteArray& data, Use use, QPdfDocument::Error* status = nullptr) {
        QTemporaryFile file(QDir::temp().filePath("opencad-ai-XXXXXX.pdf"));
        if (!file.open() || file.write(data) != data.size()) {
            return false;
        }
        file.close(); // kept on disk until `file` goes; closed, so Qt Pdf can open it on every platform
        QPdfDocument doc;
        const QPdfDocument::Error loaded = doc.load(file.fileName());
        if (loaded != QPdfDocument::Error::None || doc.pageCount() <= 0) {
            if (status != nullptr) {
                *status = loaded != QPdfDocument::Error::None ? loaded : QPdfDocument::Error::InvalidFileFormat; // no pages
            }
            return false;
        }
        use(doc);
        doc.close();
        return true;
    }

    QImage renderPage(QPdfDocument& doc, const int page, const int longEdge) {
        const QSizeF points = doc.pagePointSize(page);
        if (points.isEmpty()) {
            return {};
        }
        const double scale = longEdge / qMax(points.width(), points.height());
        const QSize size = (points * scale).toSize();
        const QImage rendered = doc.render(page, size);
        QImage paper(size, QImage::Format_RGB32); // white under the transparent parts, as on paper
        paper.fill(Qt::white);
        QPainter painter(&paper);
        painter.drawImage(0, 0, rendered);
        painter.end();
        return paper;
    }
#endif

    bool readPdf(const QByteArray& data, LC_AIAttachment& out, QString& error) {
        using namespace LC_AIAttachments;
        if (!data.startsWith("%PDF")) {
            error = QObject::tr("%1 is not a PDF file.").arg(out.name);
            return false;
        }
        if (data.size() > MAX_PDF_BYTES) {
            error = QObject::tr("%1 is %2; PDFs can be at most %3.").arg(out.name, sizeText(data.size()), sizeText(MAX_PDF_BYTES));
            return false;
        }
        // A damaged PDF is refused here: sent, it fails the whole message (Gemini answers a bare "HTTP 400"), and a
        // retry puts it back in the box to fail again.
#ifdef LC_AI_PDF
        QPdfDocument::Error status = QPdfDocument::Error::None;
        const bool opened = withPdf(data, [&out](QPdfDocument& doc) {
            out.pages = doc.pageCount();
            out.preview = renderPage(doc, 0, PDF_PREVIEW_HEIGHT);
        }, &status);
        if (!opened && (status == QPdfDocument::Error::IncorrectPassword || status == QPdfDocument::Error::UnsupportedSecurityScheme)) {
            error = QObject::tr("%1 is password-protected; the AI cannot read it.").arg(out.name);
            return false;
        }
        if (!opened && status != QPdfDocument::Error::None) {
            error = QObject::tr("%1 cannot be read (damaged PDF).").arg(out.name);
            return false;
        }
#else
        // without Qt Pdf, at least a cut-off file: a PDF ends with %%EOF (some writers add a few bytes after it)
        if (!data.right(1024).contains("%%EOF")) {
            error = QObject::tr("%1 cannot be read (damaged PDF).").arg(out.name);
            return false;
        }
#endif
        out.kind = LC_AIAttachment::Kind::Pdf;
        out.id = newId();
        out.mimeType = "application/pdf";
        out.data = data;
        return true;
    }
}

bool LC_AIAttachments::fromFile(const QString& path, LC_AIAttachment& out, QString& error) {
    const QFileInfo info(path);
    out = LC_AIAttachment();
    out.name = info.fileName();
    if (!info.isFile()) {
        error = QObject::tr("%1 is not a file.").arg(path);
        return false;
    }
    const QString suffix = info.suffix().toLower();
    const qint64 limit = suffix == "pdf" ? MAX_PDF_BYTES : MAX_IMAGE_FILE_BYTES;
    if (info.size() > limit) {
        error = suffix == "pdf" ? QObject::tr("%1 is %2; PDFs can be at most %3.").arg(out.name, sizeText(info.size()), sizeText(limit))
                                : QObject::tr("%1 is %2; that is too big to attach.").arg(out.name, sizeText(info.size()));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("%1 cannot be read: %2").arg(out.name, file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    if (suffix == "pdf" || data.startsWith("%PDF")) {
        return readPdf(data, out, error);
    }
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true); // photos taken sideways come out upright
    const QByteArray format = reader.format().toLower();
    if (format.isEmpty()) {
        error = QObject::tr("%1 is not an image or a PDF.").arg(out.name);
        return false;
    }
    return encodeImage(reader.read(), data, format, out, error);
}

bool LC_AIAttachments::fromImage(const QImage& image, const QString& name, LC_AIAttachment& out, QString& error) {
    out = LC_AIAttachment();
    out.name = name;
    return encodeImage(image, {}, {}, out, error);
}

bool LC_AIAttachments::isAttachable(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "pdf" || IMAGE_SUFFIXES.contains(suffix);
}

QStringList LC_AIAttachments::attachableFiles(const QMimeData* mime) {
    QStringList files;
    if (mime == nullptr || !mime->hasUrls()) {
        return files;
    }
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && isAttachable(url.toLocalFile())) {
            files << url.toLocalFile();
        }
    }
    return files;
}

QString LC_AIAttachments::fileDialogFilter() {
    return QObject::tr("Images and PDFs (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.pdf);;Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp);;PDF documents (*.pdf)");
}

QString LC_AIAttachments::sizeText(const qint64 bytes) {
    if (bytes < 1024 * 1024) {
        return QObject::tr("%1 KB").arg(qMax<qint64>(1, (bytes + 512) / 1024));
    }
    return QObject::tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

bool LC_AIAttachments::canRenderPdf() {
#ifdef LC_AI_PDF
    return true;
#else
    return false;
#endif
}

QList<QByteArray> LC_AIAttachments::pageImages(const LC_AIAttachment& pdf) {
    // every turn of a run sends the whole conversation again: draw each PDF once
    static QHash<QString, QList<QByteArray>> cache;
    if (const auto it = cache.constFind(pdf.id); it != cache.constEnd()) {
        return it.value();
    }
    QList<QByteArray> pages;
#ifdef LC_AI_PDF
    withPdf(pdf.data, [&pages](QPdfDocument& doc) {
        for (int i = 0; i < qMin(doc.pageCount(), MAX_PDF_PAGE_IMAGES); ++i) {
            const QImage page = renderPage(doc, i, PDF_PAGE_EDGE);
            if (!page.isNull()) {
                pages << encode(page, "JPEG", 88);
            }
        }
    });
#endif
    if (cache.size() > 32) {
        cache.clear();
    }
    cache.insert(pdf.id, pages);
    return pages;
}

bool LC_AIAttachments::readsPdf(const QString& providerId) {
    // the first-party APIs read PDFs (text and page images); OpenAI-compatible and gateway servers mostly do not
    return providerId == "anthropic" || providerId == "openai" || providerId == "google" || providerId == "openrouter";
}

QString LC_AIAttachments::modelText(const LC_AIChatMessage& message) {
    QString text = message.text;
    QStringList files;
    for (const LC_AIAttachment& a : message.attachments) {
        if (a.fromMention) {
            continue; // the mention's own context says what it is
        }
        const QString kind = a.isImage() ? QStringLiteral("image")
                             : a.pages > 0 ? QStringLiteral("PDF, %1 page%2").arg(a.pages).arg(a.pages == 1 ? "" : "s")
                                           : QStringLiteral("PDF");
        files << QStringLiteral("%1. %2 (%3)").arg(files.size() + 1).arg(a.name, kind);
    }
    if (!files.isEmpty()) {
        text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + QStringLiteral("Attached files, in order: ") + files.join("; ") + ".";
    }
    if (!message.context.isEmpty()) {
        text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + message.context;
    }
    return text;
}

namespace {
    QString pagesNote(const LC_AIAttachment& pdf, const int drawn) {
        if (drawn == 0) {
            return QStringLiteral("[%1 is attached, but this provider cannot read PDFs and OpenCAD could not draw its pages.]").arg(pdf.name);
        }
        return QStringLiteral("%1, pages 1-%2%3, drawn as images (this provider cannot read PDFs):")
            .arg(pdf.name).arg(drawn).arg(pdf.pages > drawn ? QStringLiteral(" of %1").arg(pdf.pages) : QString());
    }
}

QJsonValue LC_AIAttachments::openAIContent(const LC_AIChatMessage& message, const QString& providerId) {
    const QString text = modelText(message);
    if (message.attachments.isEmpty()) {
        return text; // a plain string, as every OpenAI-compatible server takes it
    }
    QJsonArray parts;
    if (!text.isEmpty()) {
        parts.append(QJsonObject{{"type", "text"}, {"text", text}});
    }
    const auto imagePart = [](const QString& mime, const QByteArray& data) {
        return QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", QString("data:" + mime + ";base64," + base64(data))}}}};
    };
    for (const LC_AIAttachment& a : message.attachments) {
        if (a.isImage()) {
            parts.append(imagePart(a.mimeType, a.data));
        }
        else if (readsPdf(providerId)) {
            parts.append(QJsonObject{{"type", "file"},
                                     {"file", QJsonObject{{"filename", a.name},
                                                          {"file_data", QString("data:application/pdf;base64," + base64(a.data))}}}});
        }
        else {
            const QList<QByteArray> pages = pageImages(a);
            parts.append(QJsonObject{{"type", "text"}, {"text", pagesNote(a, pages.size())}});
            for (const QByteArray& page : pages) {
                parts.append(imagePart("image/jpeg", page));
            }
        }
    }
    return parts;
}

QJsonValue LC_AIAttachments::anthropicContent(const LC_AIChatMessage& message, const QString& providerId) {
    const QString text = modelText(message);
    if (message.attachments.isEmpty()) {
        return text;
    }
    // Anthropic reads images and documents best placed before the question
    QJsonArray content;
    const auto imageBlock = [](const QString& mime, const QByteArray& data) {
        return QJsonObject{{"type", "image"}, {"source", QJsonObject{{"type", "base64"}, {"media_type", mime}, {"data", base64(data)}}}};
    };
    for (const LC_AIAttachment& a : message.attachments) {
        if (a.isImage()) {
            content.append(imageBlock(a.mimeType, a.data));
        }
        else if (readsPdf(providerId)) {
            content.append(QJsonObject{{"type", "document"}, {"title", a.name},
                                       {"source", QJsonObject{{"type", "base64"}, {"media_type", "application/pdf"},
                                                              {"data", base64(a.data)}}}});
        }
        else {
            const QList<QByteArray> pages = pageImages(a);
            content.append(QJsonObject{{"type", "text"}, {"text", pagesNote(a, pages.size())}});
            for (const QByteArray& page : pages) {
                content.append(imageBlock("image/jpeg", page));
            }
        }
    }
    if (!text.isEmpty()) {
        content.append(QJsonObject{{"type", "text"}, {"text", text}});
    }
    return content;
}

QJsonArray LC_AIAttachments::geminiParts(const LC_AIChatMessage& message) {
    QJsonArray parts;
    for (const LC_AIAttachment& a : message.attachments) {
        // Gemini reads PDFs itself, like images
        parts.append(QJsonObject{{"inline_data", QJsonObject{{"mime_type", a.mimeType}, {"data", base64(a.data)}}}});
    }
    const QString text = modelText(message);
    if (!text.isEmpty() || parts.isEmpty()) {
        parts.append(QJsonObject{{"text", text}});
    }
    return parts;
}
