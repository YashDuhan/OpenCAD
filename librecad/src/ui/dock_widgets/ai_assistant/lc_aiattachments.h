/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIATTACHMENTS_H
#define LC_AIATTACHMENTS_H

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

struct LC_AIChatMessage;
class QMimeData;

/**
 * A file sent with a message: an image (PNG, JPEG or WebP, scaled down when it is big) or a PDF.
 * The model gets it next to the text, in the shape its provider expects.
 */
struct LC_AIAttachment {
    enum class Kind { Image, Pdf };
    Kind kind {Kind::Image};
    QString id;                // unique per attachment (PDF pages drawn as images are cached by it)
    QString name;              // the file name shown on its chip
    QString mimeType;          // image/png, image/jpeg, image/webp or application/pdf
    QByteArray data;           // the bytes that are sent
    QImage preview;            // a small picture of it: the image, or the PDF's first page (null when not drawable)
    QSize size;                // image: the size sent, in px
    int pages {0};             // PDF: number of pages (0 when unknown)
    QString note;              // e.g. "scaled down from 4000 × 3000 px"
    bool fromMention {false};  // made for an @-mention (the @view screenshot), not attached by the user
    bool isImage() const { return kind == Kind::Image; }
};

namespace LC_AIAttachments {
    constexpr int MAX_ATTACHMENTS = 10;                // per message
    constexpr int MAX_IMAGE_EDGE = 1568;               // longer edges are scaled down (what Anthropic and OpenAI read at full detail)
    constexpr qint64 MAX_IMAGE_BYTES = 3750 * 1024;    // 5 MB once base64-encoded: the smallest provider limit
    constexpr qint64 MAX_IMAGE_FILE_BYTES = 50 * 1024 * 1024; // larger image files are not even opened
    constexpr qint64 MAX_PDF_BYTES = 15 * 1024 * 1024; // Gemini takes 20 MB per request, base64 included
    constexpr int MAX_PDF_PAGE_IMAGES = 10;            // pages drawn for providers that cannot read PDFs

    /** Reads an image or a PDF file; on failure returns false and says why in `error`. */
    bool fromFile(const QString& path, LC_AIAttachment& out, QString& error);
    /** An image from the clipboard or a screenshot: PNG, or JPEG when that is too big. */
    bool fromImage(const QImage& image, const QString& name, LC_AIAttachment& out, QString& error);
    /** The image and PDF files of a drop or a paste. */
    QStringList attachableFiles(const QMimeData* mime);
    bool isAttachable(const QString& path);
    /** Filter for the paperclip's file dialog. */
    QString fileDialogFilter();
    /** "2.4 MB", "830 KB". */
    QString sizeText(qint64 bytes);

    /** Built with Qt Pdf: PDFs get a preview and can be sent as page images where a provider cannot read them. */
    bool canRenderPdf();
    /** The first pages of a PDF as JPEG images (cached per attachment). */
    QList<QByteArray> pageImages(const LC_AIAttachment& pdf);
    /** Whether a provider reads PDF documents itself (otherwise their pages go as images). */
    bool readsPdf(const QString& providerId);

    /** What the model reads for a user message: the text, the list of attached files, the context of its @-mentions. */
    QString modelText(const LC_AIChatMessage& message);
    /** OpenAI chat completions "content": plain text, or text, image_url and file parts. */
    QJsonValue openAIContent(const LC_AIChatMessage& message, const QString& providerId);
    /** Anthropic Messages "content": plain text, or image and document blocks followed by the text. */
    QJsonValue anthropicContent(const LC_AIChatMessage& message, const QString& providerId);
    /** Gemini "parts": inline_data for images and PDFs, then the text. */
    QJsonArray geminiParts(const LC_AIChatMessage& message);
}

#endif
