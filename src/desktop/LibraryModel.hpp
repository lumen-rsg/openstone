#pragma once

#include <QAbstractListModel>
#include <QMap>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <vector>

namespace openstone::desktop {

class LibraryModel final : public QAbstractListModel {
public:
    enum class Section { Basics, Processing, Audio, Drawing, Toolbox };
    enum Role { EntryRole = Qt::UserRole, CategoryRole, FavoriteRole };

    struct Category {
        Section section;
        QString name;
        QString label{};
    };

    struct Entry {
        QVariantMap item;
        QString searchText;
        Category category;
        QStringList types;
        bool insideOnly = false;
        bool favorite = false;
        QString recentKey{};
    };

    explicit LibraryModel(QObject* parent) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(matches_.size());
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= rowCount()) return {};
        const auto& entry = entries_[matches_[index.row()]];
        if (role == EntryRole) return entry.item;
        if (role == CategoryRole) return entry.category.name;
        if (role == FavoriteRole) return entry.favorite;
        return {};
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {{EntryRole, "entry"}, {CategoryRole, "category"}, {FavoriteRole, "favorite"}};
    }

    void replace(std::vector<Entry> entries)
    {
        std::ranges::stable_sort(entries, [](const auto& a, const auto& b) {
            return a.searchText.localeAwareCompare(b.searchText) < 0;
        });
        beginResetModel();
        entries_ = std::move(entries);
        updateFacets();
        updateMatches();
        endResetModel();
    }

    void filter(QString query, QString group, QString type, bool inside)
    {
        query = query.trimmed().toCaseFolded();
        if (query == query_ && group == group_ && type == type_ && inside == inside_) return;
        beginResetModel();
        query_ = std::move(query);
        group_ = std::move(group);
        type_ = std::move(type);
        inside_ = inside;
        updateMatches();
        endResetModel();
    }

    void setInside(bool inside) { filter(query_, group_, type_, inside); }
    const QVariantList& sections() const { return sections_; }
    const QStringList& types() const { return types_; }
    static QString favoritesCategory() { return QStringLiteral("@favorites"); }

    static QString recentlyUsedCategory() { return QStringLiteral("@recent"); }

    const QStringList& recentlyUsed() const { return recentlyUsed_; }
    void setRecentlyUsed(QStringList entries)
    {
        if (recentlyUsed_ == entries) return;
        const bool showingRecent = group_ == recentlyUsedCategory();
        if (showingRecent) beginResetModel();
        recentlyUsed_ = std::move(entries);
        if (showingRecent) {
            updateMatches();
            endResetModel();
        }
    }

    void setFavorite(int kind, int toolboxIndex, bool favorite)
    {
        const auto entry = std::ranges::find_if(entries_, [kind, toolboxIndex](const Entry& candidate) {
            return candidate.item.value("kind").toInt() == kind &&
                   candidate.item.value("toolboxIndex", -1).toInt() == toolboxIndex;
        });
        if (entry == entries_.end() || entry->favorite == favorite) return;

        const auto entryIndex = static_cast<std::size_t>(entry - entries_.begin());
        if (group_ != favoritesCategory()) {
            entry->favorite = favorite;
            if (const auto match = std::ranges::find(matches_, entryIndex); match != matches_.end()) {
                const auto row = static_cast<int>(match - matches_.begin());
                emit dataChanged(index(row), index(row), {FavoriteRole});
            }
            return;
        }

        const auto match = std::ranges::lower_bound(matches_, entryIndex);
        const auto row = static_cast<int>(match - matches_.begin());
        const bool wasVisible = match != matches_.end() && *match == entryIndex;
        const auto terms = query_.split(u' ', Qt::SkipEmptyParts);
        const bool visible = matchesEntry(*entry, terms, favorite);

        if (wasVisible && !visible) {
            beginRemoveRows({}, row, row);
            entry->favorite = favorite;
            matches_.erase(match);
            endRemoveRows();
        } else if (!wasVisible && visible) {
            beginInsertRows({}, row, row);
            entry->favorite = favorite;
            matches_.insert(match, entryIndex);
            endInsertRows();
        } else {
            entry->favorite = favorite;
            if (visible) emit dataChanged(index(row), index(row), {FavoriteRole});
        }
    }

private:
    void updateFacets()
    {
        const std::array titles{tr("Basics"), tr("Processing"), tr("Audio"), tr("Drawing"), tr("Toolbox")};
        std::array<QMap<QString, QString>, titles.size()> categories;
        types_.clear();
        for (const auto& entry : entries_) {
            const auto& category = entry.category;
            categories[static_cast<std::size_t>(category.section)].insert(
                category.name, category.label.isEmpty() ? category.name : category.label);
            types_.append(entry.types);
        }
        types_.removeDuplicates();
        if (!types_.contains(type_)) type_.clear();

        sections_.clear();
        bool hasGroup = group_.isEmpty() || group_ == favoritesCategory() || group_ == recentlyUsedCategory();
        for (std::size_t i = 0; i < categories.size(); ++i) {
            const auto& names = categories[i];
            if (names.isEmpty()) continue;
            hasGroup = hasGroup || names.contains(group_);
            QVariantList groups;
            for (auto group = names.cbegin(); group != names.cend(); ++group) {
                groups.push_back(QVariantMap{{"label", group.value()}, {"filter", group.key()}});
            }
            sections_.push_back(QVariantMap{{"title", titles[i]}, {"groups", groups}});
        }
        if (!hasGroup) group_.clear();
    }

    bool matchesEntry(const Entry& entry, const QStringList& terms, bool favorite) const
    {
        const bool matchesCategory = group_.isEmpty() || group_ == recentlyUsedCategory() ||
            (group_ == favoritesCategory() ? favorite : entry.category.name == group_);
        return (!entry.insideOnly || inside_) && matchesCategory && (type_.isEmpty() || entry.types.contains(type_)) &&
               std::ranges::all_of(terms, [&](const auto& term) { return entry.searchText.contains(term); });
    }

    void updateMatches()
    {
        matches_.clear();
        const auto terms = query_.split(u' ', Qt::SkipEmptyParts);
        if (group_ == recentlyUsedCategory()) {
            for (const auto& key : recentlyUsed_) {
                const auto entry = std::ranges::find(entries_, key, &Entry::recentKey);
                if (entry != entries_.end() && matchesEntry(*entry, terms, entry->favorite)) {
                    matches_.push_back(static_cast<std::size_t>(entry - entries_.begin()));
                }
            }
            return;
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (matchesEntry(entries_[i], terms, entries_[i].favorite)) matches_.push_back(i);
        }
    }

    std::vector<Entry> entries_;
    std::vector<std::size_t> matches_;
    QVariantList sections_;
    QStringList types_;
    QStringList recentlyUsed_;
    QString query_, group_, type_;
    bool inside_ = false;
};

}
