/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aicredentials.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include "lc_aiprovider.h"

namespace {
    const QString PROVIDERS = QStringLiteral("providers");
    const QString SELECTED = QStringLiteral("selectedProvider");
    const QString KEY = QStringLiteral("key");
    const QString MODEL = QStringLiteral("model");
    const QString BASE_URL = QStringLiteral("baseUrl");
    const QString FAVORITES = QStringLiteral("favoriteModels");
    const QString RECENT = QStringLiteral("recentModels");
    constexpr int MAX_RECENT = 6;
}

LC_AICredentials& LC_AICredentials::instance() {
    static LC_AICredentials credentials;
    return credentials;
}

LC_AICredentials::LC_AICredentials() {
    load();
}

QString LC_AICredentials::filePath() const {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("ai-auth.json");
}

void LC_AICredentials::load() {
    QFile file(filePath());
    if (file.open(QIODevice::ReadOnly)) {
        m_data = QJsonDocument::fromJson(file.readAll()).object();
    }
}

void LC_AICredentials::save() const {
    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    // owner-only before any secret is written
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
    file.commit();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QString LC_AICredentials::providerValue(const QString& providerId, const QString& field) const {
    return m_data.value(PROVIDERS).toObject().value(providerId).toObject().value(field).toString();
}

void LC_AICredentials::setProviderValue(const QString& providerId, const QString& field, const QString& value) {
    QJsonObject providers = m_data.value(PROVIDERS).toObject();
    QJsonObject entry = providers.value(providerId).toObject();
    if (value.isEmpty()) {
        entry.remove(field);
    }
    else {
        entry.insert(field, value);
    }
    if (entry.isEmpty()) {
        providers.remove(providerId);
    }
    else {
        providers.insert(providerId, entry);
    }
    m_data.insert(PROVIDERS, providers);
    save();
}

QString LC_AICredentials::selectedProvider() const {
    const QString id = m_data.value(SELECTED).toString();
    if (LC_AIProvider::find(id) != nullptr) {
        return id;
    }
    // first provider that already has a key (saved or from the environment), else Anthropic
    for (const LC_AIProvider& p : LC_AIProvider::all()) {
        if (!p.keyOptional && keySource(p.id) != KeySource::None) {
            return p.id;
        }
    }
    return LC_AIProvider::all().first().id;
}

void LC_AICredentials::setSelectedProvider(const QString& providerId) {
    m_data.insert(SELECTED, providerId);
    save();
}

QString LC_AICredentials::apiKey(const QString& providerId) const {
    const QString saved = providerValue(providerId, KEY);
    if (!saved.isEmpty()) {
        return saved;
    }
    const LC_AIProvider* provider = LC_AIProvider::find(providerId);
    if (provider != nullptr && !provider->envVar.isEmpty()) {
        return qEnvironmentVariable(provider->envVar.toUtf8().constData()).trimmed();
    }
    return {};
}

LC_AICredentials::KeySource LC_AICredentials::keySource(const QString& providerId) const {
    if (!providerValue(providerId, KEY).isEmpty()) {
        return KeySource::Saved;
    }
    return apiKey(providerId).isEmpty() ? KeySource::None : KeySource::Environment;
}

void LC_AICredentials::setApiKey(const QString& providerId, const QString& key) {
    setProviderValue(providerId, KEY, key.trimmed());
}

void LC_AICredentials::removeApiKey(const QString& providerId) {
    setProviderValue(providerId, KEY, QString());
}

QString LC_AICredentials::model(const QString& providerId) const {
    const QString saved = providerValue(providerId, MODEL);
    if (!saved.isEmpty()) {
        return saved;
    }
    const LC_AIProvider* provider = LC_AIProvider::find(providerId);
    return provider != nullptr ? provider->defaultModel : QString();
}

void LC_AICredentials::setModel(const QString& providerId, const QString& model) {
    setProviderValue(providerId, MODEL, model.trimmed());
}

QString LC_AICredentials::baseUrl(const QString& providerId) const {
    const QString saved = providerValue(providerId, BASE_URL);
    if (!saved.isEmpty()) {
        return saved;
    }
    const LC_AIProvider* provider = LC_AIProvider::find(providerId);
    return provider != nullptr ? provider->baseUrl : QString();
}

void LC_AICredentials::setBaseUrl(const QString& providerId, const QString& baseUrl) {
    QString url = baseUrl.trimmed();
    while (url.endsWith('/')) {
        url.chop(1);
    }
    const LC_AIProvider* provider = LC_AIProvider::find(providerId);
    if (provider != nullptr && url == provider->baseUrl && !provider->customBaseUrl) {
        url.clear(); // store only overrides (a custom endpoint is kept: saving it is what connects it)
    }
    setProviderValue(providerId, BASE_URL, url);
}

bool LC_AICredentials::hasSavedBaseUrl(const QString& providerId) const {
    return !providerValue(providerId, BASE_URL).isEmpty();
}

void LC_AICredentials::reload() {
    m_data = QJsonObject();
    load();
}

bool LC_AICredentials::isConnected(const QString& providerId) const {
    if (keySource(providerId) != KeySource::None) {
        return true;
    }
    const LC_AIProvider* provider = LC_AIProvider::find(providerId);
    return provider != nullptr && provider->keyOptional && hasSavedBaseUrl(providerId);
}

QStringList LC_AICredentials::favoriteModels() const {
    QStringList ids;
    for (const QJsonValue& v : m_data.value(FAVORITES).toArray()) {
        ids << v.toString();
    }
    return ids;
}

void LC_AICredentials::setFavoriteModels(const QStringList& ids) {
    m_data.insert(FAVORITES, QJsonArray::fromStringList(ids));
    save();
}

QStringList LC_AICredentials::recentModels() const {
    QStringList ids;
    for (const QJsonValue& v : m_data.value(RECENT).toArray()) {
        ids << v.toString();
    }
    return ids;
}

void LC_AICredentials::addRecentModel(const QString& id) {
    QStringList ids = recentModels();
    ids.removeAll(id);
    ids.prepend(id);
    m_data.insert(RECENT, QJsonArray::fromStringList(ids.mid(0, MAX_RECENT)));
    save();
}

void LC_AICredentials::setSetting(const QString& key, const QJsonValue& value) {
    m_data.insert(key, value);
    save();
}
