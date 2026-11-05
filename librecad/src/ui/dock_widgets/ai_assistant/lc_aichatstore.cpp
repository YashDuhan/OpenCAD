/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aichatstore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace {
    const QString UNSAVED_PREFIX = QStringLiteral("unsaved:");
    const QString INDEX_FILE = QStringLiteral("index.json");
    constexpr int FILE_VERSION = 1;
    constexpr int MAX_TITLE = 80;
    constexpr int MAX_QUESTION_SEARCH = 160;   // characters of each question kept for the search (in index.json)
    constexpr int MAX_SEARCH_TEXT = 1200;
    constexpr int PNG_GOOD_ENOUGH = 96 * 1024; // a PNG this small is kept; a larger one is tried as JPEG too
    constexpr int MAX_IMAGE_CACHE = 256;
    const QString DROPPED_IMAGE = QStringLiteral("(An older screenshot, not kept in the saved chat.)");

    QString roleName(const LC_AIChatMessage::Role role) {
        switch (role) {
            case LC_AIChatMessage::Role::Assistant:
                return QStringLiteral("assistant");
            case LC_AIChatMessage::Role::ToolResult:
                return QStringLiteral("tool");
            case LC_AIChatMessage::Role::User:
                break;
        }
        return QStringLiteral("user");
    }

    QString dateString(const QDateTime& utc) {
        return utc.toUTC().toString(Qt::ISODateWithMs);
    }

    QDateTime dateFrom(const QJsonValue& value) {
        QDateTime t = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
        if (!t.isValid()) {
            t = QDateTime::fromString(value.toString(), Qt::ISODate);
        }
        return t.toUTC();
    }

    /** The chat files hold what the user and the AI said about their drawings: readable by their owner only. */
    void ownerOnly(const QString& path) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    bool writeJson(const QString& path, const QJsonObject& json) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning("AI chat history: cannot write %s: %s", qPrintable(path), qPrintable(file.errorString()));
            return false;
        }
        file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
        if (!file.commit()) {
            qWarning("AI chat history: cannot write %s: %s", qPrintable(path), qPrintable(file.errorString()));
            return false;
        }
        ownerOnly(path);
        return true;
    }
}

LC_AIChatStore& LC_AIChatStore::instance() {
    static LC_AIChatStore store;
    return store;
}

LC_AIChatStore::LC_AIChatStore() {
    m_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ai-chats");
    loadIndex();
}

QString LC_AIChatStore::chatFile(const QString& id) const {
    return m_dir + "/" + id + ".json";
}

QString LC_AIChatStore::pathKey(const QString& fileName) {
    if (fileName.isEmpty()) {
        return {};
    }
    // the same file reached through a link is the same drawing
    const QFileInfo info(fileName);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

QString LC_AIChatStore::newUnsavedKey() {
    return UNSAVED_PREFIX + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool LC_AIChatStore::isUnsavedKey(const QString& key) {
    return key.startsWith(UNSAVED_PREFIX);
}

// ------------------------------------------------------------------------------------------------ index

QJsonObject LC_AIChatStore::infoToJson(const LC_AIChatInfo& info) {
    QJsonObject o{{"id", info.id},
                  {"drawing", info.drawing},
                  {"drawingName", info.drawingName},
                  {"title", info.title},
                  {"created", dateString(info.created)},
                  {"updated", dateString(info.updated)},
                  {"count", info.count},
                  {"questions", info.questions}};
    if (info.renamed) {
        o.insert("renamed", true);
    }
    return o;
}

LC_AIChatInfo LC_AIChatStore::infoFromJson(const QJsonObject& json) {
    LC_AIChatInfo info;
    info.id = json.value("id").toString();
    info.drawing = json.value("drawing").toString();
    info.drawingName = json.value("drawingName").toString();
    info.title = json.value("title").toString();
    info.renamed = json.value("renamed").toBool();
    info.created = dateFrom(json.value("created"));
    info.updated = dateFrom(json.value("updated"));
    info.count = json.value("count").toInt();
    info.questions = json.value("questions").toString();
    return info;
}

void LC_AIChatStore::loadIndex() {
    QFile index(m_dir + "/" + INDEX_FILE);
    if (index.open(QIODevice::ReadOnly)) {
        for (const QJsonValue& v : QJsonDocument::fromJson(index.readAll()).object().value("chats").toArray()) {
            const LC_AIChatInfo info = infoFromJson(v.toObject());
            if (!info.id.isEmpty()) {
                m_chats.insert(info.id, info);
            }
        }
    }
    // the folder is the truth: chats the index does not list (written by another OpenCAD, or before a crash)
    // are read once, and entries whose file is gone are dropped
    bool changed = false;
    QSet<QString> onDisk;
    const QStringList files = QDir(m_dir).entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& file : files) {
        if (file == INDEX_FILE) {
            continue;
        }
        const QString id = file.chopped(5);
        onDisk.insert(id);
        if (!m_chats.contains(id)) {
            const QJsonObject chat = readChat(id);
            if (!chat.isEmpty()) {
                LC_AIChatInfo info = infoFromJson(chat);
                info.id = id;
                m_chats.insert(id, info);
                changed = true;
            }
        }
    }
    for (auto it = m_chats.begin(); it != m_chats.end();) {
        if (!onDisk.contains(it.key())) {
            it = m_chats.erase(it);
            changed = true;
        }
        else {
            ++it;
        }
    }
    if (changed) {
        writeIndex();
    }
}

void LC_AIChatStore::writeIndex() const {
    if (!QDir().mkpath(m_dir)) {
        return;
    }
    QJsonArray chats;
    for (const LC_AIChatInfo& info : allChats()) {
        chats.append(infoToJson(info));
    }
    writeJson(m_dir + "/" + INDEX_FILE, QJsonObject{{"version", FILE_VERSION}, {"chats", chats}});
}

QList<LC_AIChatInfo> LC_AIChatStore::allChats() const {
    QList<LC_AIChatInfo> list = m_chats.values();
    std::sort(list.begin(), list.end(), [](const LC_AIChatInfo& a, const LC_AIChatInfo& b) {
        return a.updated != b.updated ? a.updated > b.updated : a.id < b.id;
    });
    return list;
}

QList<LC_AIChatInfo> LC_AIChatStore::chats(const QString& drawing) const {
    QList<LC_AIChatInfo> list;
    for (const LC_AIChatInfo& info : allChats()) {
        if (info.drawing == drawing) {
            list << info;
        }
    }
    return list;
}

QString LC_AIChatStore::latest(const QString& drawing) const {
    const QList<LC_AIChatInfo> list = chats(drawing);
    return list.isEmpty() ? QString() : list.first().id;
}

// ------------------------------------------------------------------------------------------------ chats

QJsonObject LC_AIChatStore::readChat(const QString& id) const {
    QFile file(chatFile(id));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool LC_AIChatStore::writeChat(const LC_AIChatInfo& info, const QJsonArray& messages) const {
    if (!QDir().mkpath(m_dir)) {
        qWarning("AI chat history: cannot create %s", qPrintable(m_dir));
        return false;
    }
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QJsonObject json = infoToJson(info);
    json.insert("version", FILE_VERSION);
    json.insert("messages", messages);
    return writeJson(chatFile(info.id), json);
}

void LC_AIChatStore::describe(LC_AIChatInfo& info, const QList<LC_AIChatMessage>& messages) {
    QString first;
    QStringList questions;
    int count = 0;
    int length = 0;
    for (const LC_AIChatMessage& m : messages) {
        if (m.role == LC_AIChatMessage::Role::User) {
            ++count;
            const QString question = m.text.simplified();
            if (first.isEmpty()) {
                // the first line of the first question names the chat
                for (const QString& line : m.text.split('\n')) {
                    if (!line.trimmed().isEmpty()) {
                        first = line.simplified();
                        break;
                    }
                }
            }
            if (length < MAX_SEARCH_TEXT) {
                questions << question.left(MAX_QUESTION_SEARCH);
                length += qMin(static_cast<int>(question.size()), MAX_QUESTION_SEARCH);
            }
        }
        else if (m.role == LC_AIChatMessage::Role::Assistant && !m.text.trimmed().isEmpty()) {
            ++count;
        }
    }
    if (!info.renamed) {
        info.title = first.size() > MAX_TITLE ? first.left(MAX_TITLE - 1).trimmed() + QString::fromUtf8("…") : first;
    }
    info.count = count;
    info.questions = questions.join('\n');
}

QJsonObject LC_AIChatStore::smallImage(const LC_AIContent& image) {
    const QByteArray key = QCryptographicHash::hash(image.data, QCryptographicHash::Sha1) + image.mimeType.toUtf8();
    if (const auto it = m_images.constFind(key); it != m_images.constEnd()) {
        return it.value();
    }
    QByteArray bytes = image.data;
    QString mime = image.mimeType;
    QImage picture;
    if (picture.loadFromData(image.data)) {
        if (picture.width() > MAX_IMAGE_EDGE || picture.height() > MAX_IMAGE_EDGE) {
            picture = picture.scaled(MAX_IMAGE_EDGE, MAX_IMAGE_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        // CAD screenshots are mostly flat colour and thin lines: PNG keeps them sharp and is usually small;
        // a busy one goes as JPEG when that is smaller
        QBuffer png;
        png.open(QIODevice::WriteOnly);
        picture.save(&png, "PNG");
        bytes = png.data();
        mime = QStringLiteral("image/png");
        if (bytes.size() > PNG_GOOD_ENOUGH) {
            QBuffer jpeg;
            jpeg.open(QIODevice::WriteOnly);
            picture.convertToFormat(QImage::Format_RGB888).save(&jpeg, "JPEG", 82);
            if (!jpeg.data().isEmpty() && jpeg.data().size() < bytes.size()) {
                bytes = jpeg.data();
                mime = QStringLiteral("image/jpeg");
            }
        }
    }
    const QJsonObject saved{{"type", "image"}, {"mimeType", mime}, {"data", QString::fromLatin1(bytes.toBase64())}};
    if (m_images.size() >= MAX_IMAGE_CACHE) {
        m_images.clear();
    }
    m_images.insert(key, saved);
    return saved;
}

QJsonArray LC_AIChatStore::messagesToJson(const QList<LC_AIChatMessage>& messages) {
    int images = 0;
    for (const LC_AIChatMessage& m : messages) {
        for (const LC_AIContent& c : m.result.content) {
            images += c.type == LC_AIContent::Type::Image ? 1 : 0;
        }
    }
    int dropped = qMax(0, images - MAX_IMAGES); // the oldest screenshots go first

    QJsonArray out;
    for (const LC_AIChatMessage& m : messages) {
        QJsonObject o{{"role", roleName(m.role)}};
        if (!m.text.isEmpty()) {
            o.insert("text", m.text);
        }
        switch (m.role) {
            case LC_AIChatMessage::Role::User:
                break;
            case LC_AIChatMessage::Role::Assistant: {
                if (!m.origin.isEmpty()) {
                    o.insert("origin", m.origin);
                }
                if (!m.raw.isEmpty()) {
                    o.insert("raw", m.raw); // thinking signatures and the like: the same model needs them back
                }
                QJsonArray calls;
                for (const LC_AIToolCall& c : m.toolCalls) {
                    QJsonObject call{{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}};
                    if (!c.argumentsError.isEmpty()) {
                        call.insert("argumentsError", c.argumentsError);
                    }
                    calls.append(call);
                }
                if (!calls.isEmpty()) {
                    o.insert("toolCalls", calls);
                }
                break;
            }
            case LC_AIChatMessage::Role::ToolResult: {
                o.insert("toolCallId", m.toolCallId);
                o.insert("toolName", m.toolName);
                if (m.result.isError) {
                    o.insert("isError", true);
                }
                QJsonArray content;
                for (const LC_AIContent& c : m.result.content) {
                    if (c.type == LC_AIContent::Type::Text) {
                        content.append(QJsonObject{{"type", "text"}, {"text", c.text}});
                    }
                    else if (dropped > 0) {
                        --dropped;
                        content.append(QJsonObject{{"type", "text"}, {"text", DROPPED_IMAGE}});
                    }
                    else {
                        content.append(smallImage(c));
                    }
                }
                o.insert("content", content);
                break;
            }
        }
        out.append(o);
    }
    return out;
}

LC_AIChatMessage LC_AIChatStore::messageFromJson(const QJsonObject& json) {
    LC_AIChatMessage m;
    const QString role = json.value("role").toString();
    m.text = json.value("text").toString();
    if (role == "assistant") {
        m.role = LC_AIChatMessage::Role::Assistant;
        m.origin = json.value("origin").toString();
        m.raw = json.value("raw").toArray();
        for (const QJsonValue& v : json.value("toolCalls").toArray()) {
            const QJsonObject c = v.toObject();
            m.toolCalls.append({c.value("id").toString(), c.value("name").toString(), c.value("arguments").toObject(),
                                c.value("argumentsError").toString()});
        }
    }
    else if (role == "tool") {
        m.role = LC_AIChatMessage::Role::ToolResult;
        m.toolCallId = json.value("toolCallId").toString();
        m.toolName = json.value("toolName").toString();
        m.result.isError = json.value("isError").toBool();
        for (const QJsonValue& v : json.value("content").toArray()) {
            const QJsonObject c = v.toObject();
            if (c.value("type").toString() == "image") {
                m.result.content.append(LC_AIContent::makeImage(QByteArray::fromBase64(c.value("data").toString().toLatin1()),
                                                                c.value("mimeType").toString()));
            }
            else {
                m.result.content.append(LC_AIContent::makeText(c.value("text").toString()));
            }
        }
    }
    else {
        m.role = LC_AIChatMessage::Role::User;
    }
    return m;
}

QList<LC_AIChatMessage> LC_AIChatStore::load(const QString& id) const {
    QList<LC_AIChatMessage> messages;
    for (const QJsonValue& v : readChat(id).value("messages").toArray()) {
        messages.append(messageFromJson(v.toObject()));
    }
    return messages;
}

QString LC_AIChatStore::save(const QString& id, const QString& drawing, const QString& drawingName,
                             const QList<LC_AIChatMessage>& messages) {
    LC_AIChatInfo info = m_chats.value(id);
    if (id.isEmpty() || !m_chats.contains(id)) {
        info = LC_AIChatInfo();
        info.id = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
        info.created = QDateTime::currentDateTimeUtc();
    }
    info.drawing = drawing;
    info.drawingName = drawingName;
    info.updated = QDateTime::currentDateTimeUtc();
    describe(info, messages);
    if (writeChat(info, messagesToJson(messages))) {
        m_chats.insert(info.id, info);
        writeIndex();
        emit changed();
    }
    return info.id;
}

void LC_AIChatStore::rename(const QString& id, const QString& title) {
    if (!m_chats.contains(id)) {
        return;
    }
    const QJsonObject file = readChat(id);
    const QJsonArray messages = file.value("messages").toArray();
    LC_AIChatInfo info = m_chats.value(id);
    const QString name = title.simplified();
    info.renamed = !name.isEmpty();
    if (info.renamed) {
        info.title = name.left(200);
    }
    else { // back to the first question
        QList<LC_AIChatMessage> list;
        for (const QJsonValue& v : messages) {
            list.append(messageFromJson(v.toObject()));
        }
        describe(info, list);
    }
    if (writeChat(info, messages)) {
        m_chats.insert(id, info);
        writeIndex();
        emit changed();
    }
}

void LC_AIChatStore::remove(const QString& id) {
    if (!m_chats.contains(id)) {
        return;
    }
    QFile::remove(chatFile(id));
    m_chats.remove(id);
    writeIndex();
    emit changed();
}

QHash<QString, QString> LC_AIChatStore::moveDrawing(const QString& from, const QString& to, const QString& toName, const bool copy) {
    QHash<QString, QString> ids;
    if (from.isEmpty() || from == to) {
        return ids;
    }
    for (const LC_AIChatInfo& old : m_chats.values()) { // a copy: copies are inserted on the way
        if (old.drawing != from) {
            continue;
        }
        const QJsonObject file = readChat(old.id);
        if (file.isEmpty()) {
            continue;
        }
        LC_AIChatInfo info = old;
        if (copy) {
            info.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        info.drawing = to;
        info.drawingName = toName;
        if (writeChat(info, file.value("messages").toArray())) {
            m_chats.insert(info.id, info);
            ids.insert(old.id, info.id);
        }
    }
    if (!ids.isEmpty()) {
        writeIndex();
        emit changed();
    }
    return ids;
}
