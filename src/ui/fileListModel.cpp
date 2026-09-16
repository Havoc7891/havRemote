// SPDX-License-Identifier: MIT

#include "ui/fileListModel.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace havremote::ui
{
  namespace
  {
    constexpr std::array LocalColumns{
        FileListColumnWidth{FileListColumn::Name, 210},
        FileListColumnWidth{FileListColumn::Size, 85},
        FileListColumnWidth{FileListColumn::Type, 100},
        FileListColumnWidth{FileListColumn::Modified, 145},
    };

    constexpr std::array RemoteColumns{
        FileListColumnWidth{FileListColumn::Name, 210},
        FileListColumnWidth{FileListColumn::Size, 85},
        FileListColumnWidth{FileListColumn::Type, 100},
        FileListColumnWidth{FileListColumn::Modified, 145},
        FileListColumnWidth{FileListColumn::Permissions, 110},
        FileListColumnWidth{FileListColumn::Owner, 110},
    };

    int KindRank(const FileListRowKind kind) noexcept
    {
      switch (kind)
      {
      case FileListRowKind::ParentDirectory:
        return 0;

      case FileListRowKind::Directory:
        return 1;

      case FileListRowKind::File:
        return 2;

      case FileListRowKind::Symlink:
        return 3;

      case FileListRowKind::Other:
        return 4;
      }

      return 4;
    }

    template <typename Value>
    int CompareValues(const Value &left, const Value &right) noexcept
    {
      if (left < right)
      {
        return -1;
      }

      if (right < left)
      {
        return 1;
      }

      return 0;
    }

    template <typename Value>
    int CompareOptional(const std::optional<Value> &left,
                        const std::optional<Value> &right) noexcept
    {
      if (left && right)
      {
        return CompareValues(*left, *right);
      }

      if (left)
      {
        return 1;
      }

      if (right)
      {
        return -1;
      }

      return 0;
    }

    int CompareByColumn(const FileListRow &left,
                        const FileListRow &right,
                        const FileListColumn column,
                        const FileListTextComparator compareText)
    {
      switch (column)
      {
      case FileListColumn::Name:
        return compareText(left.name, right.name);

      case FileListColumn::Size:
        return CompareOptional(left.rawSize, right.rawSize);

      case FileListColumn::Type:
      {
        const auto kindComparison = CompareValues(KindRank(left.kind), KindRank(right.kind));

        if (kindComparison != 0 || left.kind != FileListRowKind::File)
        {
          return kindComparison;
        }

        return compareText(FileListExtension(left.name),
                           FileListExtension(right.name));
      }

      case FileListColumn::Modified:
        return CompareOptional(left.modifiedAt, right.modifiedAt);

      case FileListColumn::Permissions:
        return CompareOptional(left.rawPermissions, right.rawPermissions);

      case FileListColumn::Owner:
        return compareText(left.owner, right.owner);
      }

      return 0;
    }

    bool HasMetadata(const FileListRow &row,
                     const FileListColumn column) noexcept
    {
      switch (column)
      {
      case FileListColumn::Size:
        return row.rawSize.has_value();

      case FileListColumn::Modified:
        return row.modifiedAt.has_value();

      case FileListColumn::Permissions:
        return row.rawPermissions.has_value();

      case FileListColumn::Owner:
        return !row.owner.empty();

      case FileListColumn::Name:
      case FileListColumn::Type:
        return true;
      }

      return true;
    }
  } // namespace

  std::string_view FileListExtension(
      const std::string_view filename) noexcept
  {
    const auto dot = filename.rfind('.');

    if (dot == std::string_view::npos || dot == 0 ||
        dot + 1U == filename.size())
    {
      return {};
    }

    return filename.substr(dot + 1U);
  }

  FileListColumnWidths NormalizeFileListColumnWidths(
      const FileListColumnProfile profile,
      const std::span<const FileListColumnWidth> widths)
  {
    const auto normalize = [&widths](const auto &defaults)
    {
      FileListColumnWidths result;
      result.reserve(defaults.size());

      std::unordered_set<FileListColumn> consumed;

      for (const auto &fallback : defaults)
      {
        auto selected = fallback;
        const auto supplied = std::ranges::find(widths, fallback.column, &FileListColumnWidth::column);

        if (supplied != widths.end() &&
            consumed.insert(supplied->column).second)
        {
          selected.widthDips = std::clamp(
              supplied->widthDips,
              MinimumFileListColumnWidthDips,
              MaximumFileListColumnWidthDips);
        }

        result.push_back(selected);
      }

      return result;
    };

    return profile == FileListColumnProfile::Remote
               ? normalize(RemoteColumns)
               : normalize(LocalColumns);
  }

  std::string FormatFilePermissions(
      const std::optional<std::uint32_t> permissions)
  {
    if (!permissions)
    {
      return {};
    }

    const auto mode = *permissions;

    std::string result(9U, '-');

    if ((mode & 0400U) != 0U)
    {
      result[0] = 'r';
    }

    if ((mode & 0200U) != 0U)
    {
      result[1] = 'w';
    }

    if ((mode & 04000U) != 0U)
    {
      result[2] = (mode & 0100U) != 0U ? 's' : 'S';
    }
    else if ((mode & 0100U) != 0U)
    {
      result[2] = 'x';
    }

    if ((mode & 0040U) != 0U)
    {
      result[3] = 'r';
    }

    if ((mode & 0020U) != 0U)
    {
      result[4] = 'w';
    }

    if ((mode & 02000U) != 0U)
    {
      result[5] = (mode & 0010U) != 0U ? 's' : 'S';
    }
    else if ((mode & 0010U) != 0U)
    {
      result[5] = 'x';
    }

    if ((mode & 0004U) != 0U)
    {
      result[6] = 'r';
    }

    if ((mode & 0002U) != 0U)
    {
      result[7] = 'w';
    }

    if ((mode & 01000U) != 0U)
    {
      result[8] = (mode & 0001U) != 0U ? 't' : 'T';
    }
    else if ((mode & 0001U) != 0U)
    {
      result[8] = 'x';
    }

    return result;
  }

  std::string FormatFileOwnerGroup(const std::string_view owner,
                                   const std::string_view group)
  {
    if (owner.empty())
    {
      return std::string{group};
    }

    if (group.empty())
    {
      return std::string{owner};
    }

    std::string result;
    result.reserve(owner.size() + 1U + group.size());
    result.append(owner);
    result.push_back('/');
    result.append(group);

    return result;
  }

  std::optional<std::uint32_t> ParseFilePermissionsOctal(
      const std::string_view text) noexcept
  {
    if (text.size() != 3U && text.size() != 4U)
    {
      return std::nullopt;
    }

    std::uint32_t result{};

    for (const unsigned char character : text)
    {
      if (character < '0' || character > '7')
      {
        return std::nullopt;
      }

      result = (result << 3U) |
               static_cast<std::uint32_t>(character - '0');
    }

    return result <= 07777U ? std::optional<std::uint32_t>{result}
                            : std::nullopt;
  }

  std::string FormatFilePermissionsOctal(const std::uint32_t permissions)
  {
    const auto mode = permissions & 07777U;

    std::array<char, 4> digits{};

    for (std::size_t index = digits.size(); index > 0U; --index)
    {
      digits[index - 1U] =
          static_cast<char>('0' + ((mode >> ((digits.size() - index) * 3U)) &
                                   07U));
    }

    return {digits.data(), digits.size()};
  }

  void SortFileListRows(std::vector<FileListRow> &rows,
                        const FileListColumn column,
                        const bool ascending,
                        const FileListTextComparator compareText)
  {
    if (!compareText)
    {
      return;
    }

    std::ranges::stable_sort(
        rows,
        [column, ascending, compareText](const FileListRow &left,
                                         const FileListRow &right)
        {
          const bool leftParent =
              left.kind == FileListRowKind::ParentDirectory;
          const bool rightParent =
              right.kind == FileListRowKind::ParentDirectory;

          if (leftParent != rightParent)
          {
            return leftParent;
          }

          const bool leftDirectory = left.kind == FileListRowKind::Directory;
          const bool rightDirectory = right.kind == FileListRowKind::Directory;
          if (leftDirectory != rightDirectory)
          {
            return leftDirectory;
          }

          // Empty metadata cells remain at the end for both sort directions
          const bool leftHasMetadata = HasMetadata(left, column);
          const bool rightHasMetadata = HasMetadata(right, column);
          if (leftHasMetadata != rightHasMetadata)
          {
            return leftHasMetadata;
          }

          int comparison = CompareByColumn(left, right, column, compareText);

          if (comparison == 0 && column != FileListColumn::Name)
          {
            comparison = compareText(left.name, right.name);
          }

          if (comparison == 0)
          {
            comparison = left.stableIdentity.compare(right.stableIdentity);
          }

          return ascending ? comparison < 0 : comparison > 0;
        });
  }
} // namespace havremote::ui
