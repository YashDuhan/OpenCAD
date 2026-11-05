/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICREDENTIALS_H
#define LC_AICREDENTIALS_H

#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * Stores pasted API keys and per-provider settings in ai-auth.json in the user's config directory
 * (like auth.json in the Sorvex/Cypher harness). The file is readable by its owner only and holds
 * secrets: never commit or share it. A saved key wins over the provider's environment variable.
 */
class LC_AICredentials {
public:
    enum class KeySource { None, Saved, Environment };

    static LC_AICredentials& instance();

    QString filePath() const;

    QString selectedProvider() const;
    void setSelectedProvider(const QString& providerId);

    QString apiKey(const QString& providerId) const;
    KeySource keySource(const QString& providerId) const;
    void setApiKey(const QString& providerId, const QString& key);
    void removeApiKey(const QString& providerId);

    QString model(const QString& providerId) const;     // saved model, else the provider default
    void setModel(const QString& providerId, const QString& model);
    QString baseUrl(const QString& providerId) const;   // saved base URL, else the provider default
    void setBaseUrl(const QString& providerId, const QString& baseUrl);
    bool hasSavedBaseUrl(const QString& providerId) const;

    /** Has a key (saved or from the environment), or is a key-less endpoint the user has set up. */
    bool isConnected(const QString& providerId) const;

    /** Models pinned with the star in the model picker, and the last models used: "provider/model" ids. */
    QStringList favoriteModels() const;
    void setFavoriteModels(const QStringList& ids);
    QStringList recentModels() const;
    void addRecentModel(const QString& id);

    /** Other AI settings kept in the same file (approval mode, tool permissions). */
    QJsonValue setting(const QString& key) const { return m_data.value(key); }
    void setSetting(const QString& key, const QJsonValue& value);

    /** Re-reads the file (e.g. after it was edited outside OpenCAD). */
    void reload();

private:
    LC_AICredentials();
    QString providerValue(const QString& providerId, const QString& field) const;
    void setProviderValue(const QString& providerId, const QString& field, const QString& value);
    void load();
    void save() const;

    QJsonObject m_data;
};

#endif
