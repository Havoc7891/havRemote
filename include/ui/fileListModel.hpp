// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_FILE_LIST_MODEL_HPP
#define HAVREMOTE_INCLUDE_UI_FILE_LIST_MODEL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace havremote::ui
{
  enum class FileListColumnProfile
  {
    Local,
    Remote,
  };

  enum class FileListRowKind
  {
    ParentDirectory,
    Directory,
    File,
    Symlink,
    Other,
  };

  enum class FileListColumn : int
  {
    Name = 0,
    Size,
    Type,
    Modified,
    Permissions,
    Owner,
  };

  inline constexpr std::uint32_t MinimumFileListColumnWidthDips = 32;
  inline constexpr std::uint32_t MaximumFileListColumnWidthDips = 4096;

  struct FileListColumnWidth final
  {
    FileListColumn column{FileListColumn::Name};
    std::uint32_t widthDips{};

    friend bool operator==(const FileListColumnWidth &,
                           const FileListColumnWidth &) = default;
  };

  using FileListColumnWidths = std::vector<FileListColumnWidth>;

  // Returns one entry for every column in the selected profile, in logical
  // column order. Unknown and duplicate input columns are ignored, omitted
  // columns use their defaults, and accepted widths are clamped to a usable
  // range. This keeps untrusted persisted layout data away from wxListCtrl.
  [[nodiscard]] FileListColumnWidths NormalizeFileListColumnWidths(
      FileListColumnProfile profile,
      std::span<const FileListColumnWidth> widths);

  // Returns the final filename suffix without its dot, preserving its bytes
  // and case. A leading dot alone (such as .gitignore), a trailing dot, or no
  // dot produces an empty view. The input is a filename, not a full path.
  [[nodiscard]] std::string_view FileListExtension(
      std::string_view filename) noexcept;

  // A complete presentation snapshot for one file-list row. The stable identity
  // must uniquely identify the entry within a snapshot independently of its
  // current visual position (for example, a full local path or the raw bytes of
  // a remote path).
  struct FileListRow
  {
    std::string stableIdentity;
    // Navigation rows have no backing entry and therefore no source index
    std::optional<std::size_t> sourceIndex;
    FileListRowKind kind{FileListRowKind::Other};
    std::string name;
    std::string size;
    std::string type;
    std::string modified;
    std::string permissions;
    std::string owner;
    std::optional<std::uint64_t> rawSize;
    std::optional<std::chrono::system_clock::time_point> modifiedAt;
    std::optional<std::uint32_t> rawPermissions;
  };

  // Returns a negative value when left precedes right, a positive value when
  // right precedes left, and zero when both text values compare equally.
  using FileListTextComparator = int (*)(std::string_view left,
                                         std::string_view right);

  // Formats the portable permission bits as rwxrwxrwx. File-type bits supplied
  // by SFTP are ignored, while setuid, setgid, and sticky bits use the customary
  // s/S and t/T forms. Missing metadata produces an empty cell.
  [[nodiscard]] std::string FormatFilePermissions(
      std::optional<std::uint32_t> permissions);

  // Formats a remote owner and group for the shared Owner/Group column. Missing
  // metadata does not introduce a leading or trailing separator.
  [[nodiscard]] std::string FormatFileOwnerGroup(std::string_view owner,
                                                 std::string_view group);

  // Parses an exact POSIX permission mode written as three or four octal
  // digits. Three digits represent the ordinary rwx bits with no special bits.
  // Whitespace, signs, prefixes, and values outside 0000..07777 are rejected.
  [[nodiscard]] std::optional<std::uint32_t> ParseFilePermissionsOctal(
      std::string_view text) noexcept;

  // Formats all twelve portable permission bits as four octal digits
  [[nodiscard]] std::string FormatFilePermissionsOctal(
      std::uint32_t permissions);

  // A parent-directory navigation row remains first, followed by directory rows.
  // Absent metadata values remain last. Type sorts by semantic kind, then by
  // filename extension for regular files, independently of translated labels.
  void SortFileListRows(std::vector<FileListRow> &rows,
                        FileListColumn column,
                        bool ascending,
                        FileListTextComparator compareText);
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_FILE_LIST_MODEL_HPP
