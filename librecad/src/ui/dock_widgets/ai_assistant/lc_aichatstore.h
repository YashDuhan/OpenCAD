/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICHATSTORE_H
#define LC_AICHATSTORE_H

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>

#include "lc_aiinferencemanager.h"

/** What the history list shows of a saved chat. index.json keeps these, so the list opens without reading every chat. */
struct LC_AIChatInfo {
    QString id;
    QString drawing;      // absolute file path; "unsaved:<uuid>" for a drawing not saved yet; empty without a drawing
    QString drawingName;  // the file name, or the window title of an unsaved drawing
    QString title;        // the first question, or the name the user gave the chat
    bool renamed {false};
    QDateTime created;
    QDateTime updated;
    int count {0};        // questions and answers; tool calls, their results and the lines between them are not counted
    QString questions;    // the start of every question, for the search
};

/**
 * The saved AI chats: one JSON file per chat in <app data>/ai-chats/, filed under the drawing they belong to.
 * A chat keeps what continuing it needs (tool calls and results, the provider's raw blocks such as thinking
 * signatures); screenshots are scaled down and only the newest ones are kept, so the files stay small. The files
 * attached to questions (images, PDFs) are kept as they were sent, in a folder next to the chat: <id>.files/.
 */
class LC_AIChatStore : public QObject {
    Q_OBJECT
public:
    static constexpr int MAX_IMAGE_EDGE = 640; // px: saved screenshots are scaled down to this
    static constexpr int MAX_IMAGES = 40;      // per chat, the newest; older ones are left out of the file
    // per chat, the attached files of the newest questions; older ones are left out (the chat says so)
    static constexpr qint64 MAX_FILE_BYTES = 64 * 1024 * 1024;

    static LC_AIChatStore& instance();

    QString directory() const { return m_dir; }
    /** The chats of this drawing, newest first. */
    QList<LC_AIChatInfo> chats(const QString& drawing) const;
    /** Every chat, newest first. */
    QList<LC_AIChatInfo> allChats() const;
    bool contains(const QString& id) const { return m_chats.contains(id); }
    LC_AIChatInfo info(const QString& id) const { return m_chats.value(id); }
    /** The chat of this drawing that changed last, or an empty id. */
    QString latest(const QString& drawing) const;

    /** The conversation; when the chat's file cannot be read, an empty one and the reason in `error`. */
    QList<LC_AIChatMessage> load(const QString& id, QString* error = nullptr) const;
    /** Writes the conversation; an empty or unknown id makes a new chat. Returns the chat's id. */
    QString save(const QString& id, const QString& drawing, const QString& drawingName, const QList<LC_AIChatMessage>& messages);
    /** A new name for the chat; an empty one brings back the first question. */
    void rename(const QString& id, const QString& title);
    void remove(const QString& id);
    /**
     * A drawing has a new key: an unsaved drawing was saved (its chats move along), or a drawing was saved
     * under another name (its chats are copied, so the old file keeps its history too). Returns old id -> new id.
     */
    QHash<QString, QString> moveDrawing(const QString& from, const QString& to, const QString& toName, bool copy);

    /** The key of a drawing file: its absolute path, links resolved. */
    static QString pathKey(const QString& fileName);
    static QString newUnsavedKey();
    static bool isUnsavedKey(const QString& key);

    /**
     * Tool calls without a result (the run was stopped while they waited for approval, or before they ran) get one
     * that says so, marked stopped: providers refuse a conversation with unanswered calls. Returns true when it added any.
     */
    static bool closeOpenCalls(QList<LC_AIChatMessage>& messages);

signals:
    void changed();

private:
    LC_AIChatStore();
    QString chatFile(const QString& id) const;
    /** The folder of the chat's attached files. */
    QString filesDir(const QString& id) const;
    void loadIndex();
    void writeIndex() const;
    bool writeChat(const LC_AIChatInfo& info, const QJsonArray& messages) const;
    /** The chat's JSON; empty, with the reason in `error`, when its file is missing or damaged. */
    QJsonObject readChat(const QString& id, QString* error = nullptr) const;
    /** `files`: the attached files the JSON refers to (written to the chat's folder on the way). */
    QJsonArray messagesToJson(const QString& id, const QList<LC_AIChatMessage>& messages, QSet<QString>& files);
    LC_AIChatMessage messageFromJson(const QJsonObject& json, const QString& id) const;
    /** A screenshot as the file keeps it: scaled down, as PNG or JPEG, whichever is smaller. */
    QJsonObject smallImage(const LC_AIContent& image);
    /** An attached file's name in the chat's folder: a hash of its bytes, so a file attached twice is kept once. */
    QString fileNameOf(const LC_AIAttachment& attachment);
    bool writeFile(const QString& id, const QString& name, const QByteArray& data) const;
    /** An attached file as it was sent, read back from the chat's folder; false when it is gone or unreadable. */
    bool readAttachment(const QString& id, const QJsonObject& json, LC_AIAttachment& out) const;
    /** Takes the files the chat no longer refers to out of its folder. */
    void removeUnusedFiles(const QString& id, const QSet<QString>& used) const;
    bool copyFiles(const QString& from, const QString& to) const;

    static QJsonObject infoToJson(const LC_AIChatInfo& info);
    static LC_AIChatInfo infoFromJson(const QJsonObject& json);
    static void describe(LC_AIChatInfo& info, const QList<LC_AIChatMessage>& messages);

    QString m_dir;
    QHash<QString, LC_AIChatInfo> m_chats;
    QHash<QByteArray, QJsonObject> m_images; // screenshot hash -> its saved form, so a chat is not re-encoded on every save
    QHash<QString, QString> m_fileNames;     // attachment id -> its file name, so its bytes are hashed once
    mutable QHash<QString, LC_AIAttachment> m_files; // file path -> the attachment read from it, its preview drawn once
    mutable qint64 m_filesBytes {0};
};

#endif
