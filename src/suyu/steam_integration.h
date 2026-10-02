// SPDX-FileCopyrightText: 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

/// Steam library integration — adds games to Steam as non-Steam shortcuts with artwork.
/// Reads and writes Steam's binary VDF shortcuts.vdf format directly.
class SteamIntegration : public QObject {
    Q_OBJECT

public:
    explicit SteamIntegration(QObject* parent = nullptr);
    ~SteamIntegration() override;

    /// Detect whether Steam is installed.
    [[nodiscard]] bool IsSteamInstalled() const;

    /// Get the Steam userdata directory path.
    [[nodiscard]] QString GetSteamUserdataPath() const;

    /// Add a game as a non-Steam shortcut.
    bool AddGameShortcut(const QString& game_title, const QString& rom_path,
                         const QString& icon_path = {});

    /// Add suyu itself (not a specific game) as a non-Steam shortcut, using
    /// its own icon, so it - and by extension its whole library - shows up
    /// in the Steam library/overlay without needing to add each game
    /// one-by-one. Reuses AddGameShortcut with an empty rom_path.
    bool AddSuyuSelfShortcut();

    /// Remove a previously added shortcut.
    bool RemoveGameShortcut(const QString& game_title);

    /// Add, or repoint, a shortcut that runs a standalone launcher directly - an exported game -
    /// rather than suyu with a ROM. An existing shortcut for the same launcher is updated in
    /// place, so exporting again does not duplicate it. When @p replace_title is not empty,
    /// shortcuts with that title that launch anything else (suyu with the ROM) are removed so
    /// the launcher takes their place.
    bool AddLauncherShortcut(const QString& app_name, const QString& launcher_path,
                             const QString& replace_title = {});

    /// Write library artwork for a shortcut added by AddLauncherShortcut into the grid folder
    /// of the same Steam account: portrait and wide capsules, hero, logo and icon, named by
    /// the shortcut's appid. @p cover, when not null, is used for the capsules and hero in
    /// place of @p icon. Only this shortcut's files are touched, each replaced atomically.
    bool WriteLauncherArtwork(const QString& app_name, const QString& launcher_path,
                              const QImage& icon, const QImage& cover = {});

    enum class ArtworkType {
        Grid,
        Hero,
        Icon,
        Artwork,
    };

    /// Fetch artwork for a game title using public Steam Store endpoints.
    /// Downloads asynchronously; emits ArtworkFetched on completion.
    void FetchArtwork(const QString& game_title, const QString& output_path,
                      ArtworkType artwork_type = ArtworkType::Grid);

    struct SteamShortcut {
        quint32 id{};
        QString app_name;
        QString exe;
        QString start_dir;
        QString icon;
        QString shortcut_path;
        QString launch_options;
        bool is_hidden{false};
        bool allow_desktop_config{true};
        bool allow_overlay{true};
        qint32 last_play_time{0};
        QStringList tags;

        // Steam's per-shortcut artwork slots. `icon` above is the small
        // launcher icon; these are the wide images the library grid, the
        // details page and the header respectively use. They are separate keys
        // in shortcuts.vdf, not a variant of `icon`, which is why a shortcut
        // with a perfectly good grid image can still show the generic box art.
        QString grid_art;
        QString hero_art;
        QString logo_art;
    };

    /// List all currently registered shortcuts.
    [[nodiscard]] std::vector<SteamShortcut> ListShortcuts() const;

    /// Compute the Steam AppID for a non-Steam shortcut.
    [[nodiscard]] static quint32 GenerateAppId(const QString& exe, const QString& app_name);

    /// Update the artwork of an existing shortcut, creating nothing.
    ///
    /// Artwork arrives asynchronously, by which point the shortcut that was
    /// added to get the download started is already on disk without it.
    /// Re-adding it wholesale was the previous behaviour and it worked, but it
    /// re-derives the AppID from the current exe path, so a shortcut whose exe
    /// moved gets a new ID and the old entry is orphaned in the library.
    /// Returns false when no shortcut by that name exists.
    bool SetShortcutArtwork(const QString& game_title, const QString& artwork_path,
                            ArtworkType artwork_type = ArtworkType::Grid);

    /// Where a given artwork kind is cached, given its owner and Steam AppID.
    ///
    /// Fetched art is kept rather than written straight into the Steam
    /// userdata directory: the caller needs a real file path it owns, and
    /// userdata is Steam's to manage. Returns empty when there is no Steam
    /// userdata directory to key off.
    [[nodiscard]] QString GetArtworkCachePath(const QString& owner, quint64 steam_app_id,
                                             ArtworkType artwork_type) const;

    /// Filename suffix for an artwork kind, used in the cache path.
    [[nodiscard]] static QString ArtworkTypeSuffix(ArtworkType artwork_type);

signals:
    void ShortcutAdded(const QString& title);
    void ShortcutRemoved(const QString& title);
    void ArtworkFetched(const QString& title, const QString& path);
    void ArtworkFetchFailed(const QString& title, const QString& error);

private:
    /// Find the first user's shortcuts.vdf path.
    [[nodiscard]] QString FindShortcutsVdf() const;

    /// Parse Steam binary VDF shortcuts.vdf into a list of shortcuts.
    [[nodiscard]] std::vector<SteamShortcut> ParseShortcutsVdf(const QByteArray& data) const;

    /// Serialize a list of shortcuts back into binary VDF format.
    [[nodiscard]] QByteArray SerializeShortcutsVdf(const std::vector<SteamShortcut>& shortcuts) const;

    /// Write VDF byte helpers.
    void VdfWriteString(QByteArray& buf, quint8 type, const QByteArray& key,
                        const QByteArray& value) const;
    void VdfWriteUint32(QByteArray& buf, const QByteArray& key, quint32 value) const;

    [[nodiscard]] QString FindSteamPath() const;

    QString steam_path_;
    QNetworkAccessManager* network_manager_{};
};
