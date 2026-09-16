// SPDX-License-Identifier: MIT

#include "ui/fileListCtrl.hpp"

#include <wx/arrstr.h>
#include <wx/gdicmn.h>
#include <wx/vector.h>

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace havremote::ui
{
  namespace
  {
    constexpr int ColumnIndex(const FileListColumn column) noexcept
    {
      return static_cast<int>(column);
    }

    wxString FromUtf8(const std::string_view text)
    {
      return wxString::FromUTF8(text.data(), text.size());
    }

    int NaturalCompare(const std::string_view left,
                       const std::string_view right)
    {
      // Use wxWidgets' natural comparison for case-insensitive, numeric-aware file-list ordering
      return wxCmpNatural(FromUtf8(left), FromUtf8(right));
    }

    long RequiredListStyle(const long style) noexcept
    {
      constexpr long removedStyles = wxLC_MASK_TYPE | wxLC_MASK_SORT |
                                     wxLC_SINGLE_SEL | wxLC_NO_HEADER |
                                     wxLC_NO_SORT_HEADER;

      return (style & ~removedStyles) | wxLC_REPORT | wxLC_VIRTUAL | wxLC_EDIT_LABELS;
    }
  } // namespace

  struct FileListCtrl::ViewState
  {
    std::unordered_set<std::string> selectedIdentities;
    std::optional<std::string> focusedIdentity;
    std::optional<std::string> topIdentity;
  };

  FileListCtrl::FileListCtrl(wxWindow *const parent,
                             const wxWindowID id,
                             const FileListColumnProfile profile,
                             const wxPoint &position,
                             const wxSize &size,
                             const long style)
      : wxListCtrl(parent,
                   id,
                   position,
                   size,
                   RequiredListStyle(style)),
        mProfile(profile)
  {
    InsertColumn(ColumnIndex(FileListColumn::Name), wxString{},
                 wxLIST_FORMAT_LEFT, FromDIP(210));
    InsertColumn(ColumnIndex(FileListColumn::Size), wxString{},
                 wxLIST_FORMAT_RIGHT, FromDIP(85));
    InsertColumn(ColumnIndex(FileListColumn::Type), wxString{},
                 wxLIST_FORMAT_LEFT, FromDIP(100));
    InsertColumn(ColumnIndex(FileListColumn::Modified), wxString{},
                 wxLIST_FORMAT_LEFT, FromDIP(145));

    if (mProfile == FileListColumnProfile::Remote)
    {
      InsertColumn(ColumnIndex(FileListColumn::Permissions), wxString{},
                   wxLIST_FORMAT_LEFT, FromDIP(110));
      InsertColumn(ColumnIndex(FileListColumn::Owner), wxString{},
                   wxLIST_FORMAT_LEFT, FromDIP(110));
    }

    ShowSortIndicator(ColumnIndex(mSortColumn), mSortAscending);

    Bind(wxEVT_LIST_COL_CLICK, &FileListCtrl::OnColumnClick, this);
  }

  void FileListCtrl::SetColumnLabels(const FileListColumnLabels &labels)
  {
    const auto setLabel = [this](const FileListColumn column,
                                 const wxString &label)
    {
      wxListItem item;
      item.SetMask(wxLIST_MASK_TEXT);
      item.SetText(label);

      SetColumn(ColumnIndex(column), item);
    };

    setLabel(FileListColumn::Name, labels.name);
    setLabel(FileListColumn::Size, labels.size);
    setLabel(FileListColumn::Type, labels.type);
    setLabel(FileListColumn::Modified, labels.modified);

    if (mProfile == FileListColumnProfile::Remote)
    {
      setLabel(FileListColumn::Permissions, labels.permissions);
      setLabel(FileListColumn::Owner, labels.owner);
    }
  }

  void FileListCtrl::SetIcons(const FileListIcons &icons)
  {
    mFolderImage = -1;
    mFileImage = -1;
    mSymlinkImage = -1;
    mOtherImage = -1;

    wxVector<wxBitmapBundle> images;

    const auto append = [&images](const wxBitmapBundle &icon, int &index)
    {
      if (!icon.IsOk())
      {
        return;
      }

      index = static_cast<int>(images.size());

      images.push_back(icon);
    };

    append(icons.folder, mFolderImage);
    append(icons.file, mFileImage);
    append(icons.symlink, mSymlinkImage);
    append(icons.other, mOtherImage);

    SetSmallImages(images);

    RefreshRows();
  }

  void FileListCtrl::SetRows(std::vector<FileListRow> rows)
  {
    const auto state = CaptureViewState();

    mRows = std::move(rows);

    SortRows();

    SetItemCount(static_cast<long>(mRows.size()));

    RefreshRows();

    RestoreViewState(state);
  }

  void FileListCtrl::ClearRows()
  {
    SetRows({});
  }

  void FileListCtrl::SortBy(const FileListColumn column,
                            const bool ascending)
  {
    if (!HasColumn(column))
    {
      return;
    }

    const auto state = CaptureViewState();

    mSortColumn = column;

    mSortAscending = ascending;

    SortRows();

    ShowSortIndicator(ColumnIndex(mSortColumn), mSortAscending);

    RefreshRows();

    RestoreViewState(state);
  }

  const std::vector<FileListRow> &FileListCtrl::Rows() const noexcept
  {
    return mRows;
  }

  const FileListRow *FileListCtrl::RowAt(const long visualIndex) const noexcept
  {
    if (visualIndex < 0 ||
        static_cast<std::size_t>(visualIndex) >= mRows.size())
    {
      return nullptr;
    }

    return &mRows[static_cast<std::size_t>(visualIndex)];
  }

  std::optional<std::size_t> FileListCtrl::SourceIndexAt(
      const long visualIndex) const noexcept
  {
    const auto *const row = RowAt(visualIndex);

    if (!row)
    {
      return std::nullopt;
    }

    return row->sourceIndex;
  }

  std::vector<std::size_t> FileListCtrl::SelectedSourceIndices() const
  {
    std::vector<std::size_t> indices;
    indices.reserve(SelectedRowCount());

    long item = -1;

    while ((item = GetNextItem(item,
                               wxLIST_NEXT_ALL,
                               wxLIST_STATE_SELECTED)) >= 0)
    {
      if (const auto *const row = RowAt(item); row && row->sourceIndex)
      {
        indices.push_back(*row->sourceIndex);
      }
    }

    return indices;
  }

  std::optional<FileListRow> FileListCtrl::SingleSelectedRow() const
  {
    if (SelectedRowCount() != 1U)
    {
      return std::nullopt;
    }

    const long item = GetNextItem(-1,
                                  wxLIST_NEXT_ALL,
                                  wxLIST_STATE_SELECTED);

    const auto *const row = RowAt(item);

    return row ? std::optional<FileListRow>{*row} : std::nullopt;
  }

  std::size_t FileListCtrl::SelectedRowCount() const noexcept
  {
    const long count = GetSelectedItemCount();
    return count > 0 ? static_cast<std::size_t>(count) : 0U;
  }

  FileListColumn FileListCtrl::SortColumn() const noexcept
  {
    return mSortColumn;
  }

  bool FileListCtrl::IsSortAscending() const noexcept
  {
    return mSortAscending;
  }

  FileListColumnWidths FileListCtrl::ColumnWidthsDips() const
  {
    FileListColumnWidths widths;

    const int count = GetColumnCount();

    widths.reserve(count > 0 ? static_cast<std::size_t>(count) : 0U);

    for (int index = 0; index < count; ++index)
    {
      const auto width = ToDIP(GetColumnWidth(index));

      widths.push_back(FileListColumnWidth{
          .column = static_cast<FileListColumn>(index),
          .widthDips = static_cast<std::uint32_t>(std::max(0, width)),
      });
    }

    return NormalizeFileListColumnWidths(mProfile, widths);
  }

  void FileListCtrl::ApplyColumnWidthsDips(
      const std::span<const FileListColumnWidth> widths)
  {
    for (const auto &column : NormalizeFileListColumnWidths(mProfile, widths))
    {
      (void)SetColumnWidth(
          ColumnIndex(column.column),
          FromDIP(static_cast<int>(column.widthDips)));
    }
  }

  wxString FileListCtrl::OnGetItemText(const long item,
                                       const long column) const
  {
    const auto *const row = RowAt(item);

    if (!row)
    {
      return {};
    }

    switch (static_cast<FileListColumn>(column))
    {
    case FileListColumn::Name:
      return FromUtf8(row->name);

    case FileListColumn::Size:
      return FromUtf8(row->size);

    case FileListColumn::Type:
      return FromUtf8(row->type);

    case FileListColumn::Modified:
      return FromUtf8(row->modified);

    case FileListColumn::Permissions:
      return FromUtf8(row->permissions);

    case FileListColumn::Owner:
      return FromUtf8(row->owner);
    }

    return {};
  }

  int FileListCtrl::OnGetItemImage(const long item) const
  {
    const auto *const row = RowAt(item);
    if (!row)
    {
      return -1;
    }

    switch (row->kind)
    {
    case FileListRowKind::ParentDirectory:
    case FileListRowKind::Directory:
      return mFolderImage;

    case FileListRowKind::File:
      return mFileImage;

    case FileListRowKind::Symlink:
      return mSymlinkImage;

    case FileListRowKind::Other:
      return mOtherImage;
    }

    return -1;
  }

  FileListCtrl::ViewState FileListCtrl::CaptureViewState() const
  {
    ViewState state;
    state.selectedIdentities.reserve(SelectedRowCount());

    long item = -1;
    while ((item = GetNextItem(item,
                               wxLIST_NEXT_ALL,
                               wxLIST_STATE_SELECTED)) >= 0)
    {
      if (const auto *const row = RowAt(item))
      {
        state.selectedIdentities.insert(row->stableIdentity);
      }
    }

    item = GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED);
    if (const auto *const row = RowAt(item))
    {
      state.focusedIdentity = row->stableIdentity;
    }

    if (const auto *const row = RowAt(GetTopItem()))
    {
      state.topIdentity = row->stableIdentity;
    }

    return state;
  }

  void FileListCtrl::RestoreViewState(const ViewState &state)
  {
    SetItemState(-1,
                 0,
                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);

    std::unordered_map<std::string_view, long> rowsByIdentity;
    rowsByIdentity.reserve(mRows.size());

    for (std::size_t index = 0; index < mRows.size(); ++index)
    {
      rowsByIdentity.try_emplace(mRows[index].stableIdentity,
                                 static_cast<long>(index));
    }

    for (const auto &identity : state.selectedIdentities)
    {
      if (const auto found = rowsByIdentity.find(identity);
          found != rowsByIdentity.end())
      {
        SetItemState(found->second,
                     wxLIST_STATE_SELECTED,
                     wxLIST_STATE_SELECTED);
      }
    }

    if (state.focusedIdentity)
    {
      if (const auto found = rowsByIdentity.find(*state.focusedIdentity);
          found != rowsByIdentity.end())
      {
        SetItemState(found->second,
                     wxLIST_STATE_FOCUSED,
                     wxLIST_STATE_FOCUSED);
      }
    }

    if (!state.topIdentity)
    {
      return;
    }

    const auto found = rowsByIdentity.find(*state.topIdentity);
    if (found == rowsByIdentity.end())
    {
      return;
    }

    const long desiredTop = found->second;

    EnsureVisible(desiredTop);

    const long currentTop = GetTopItem();

    if (currentTop < 0 || currentTop == desiredTop)
    {
      return;
    }

    wxRect itemRectangle;

    if (!GetItemRect(currentTop, itemRectangle) || itemRectangle.height <= 0)
    {
      return;
    }

    const long long distance =
        (static_cast<long long>(desiredTop) -
         static_cast<long long>(currentTop)) *
        static_cast<long long>(itemRectangle.height);

    const auto minimum = static_cast<long long>(std::numeric_limits<int>::min());
    const auto maximum = static_cast<long long>(std::numeric_limits<int>::max());

    ScrollList(0, static_cast<int>(std::clamp(distance, minimum, maximum)));
  }

  bool FileListCtrl::HasColumn(const FileListColumn column) const noexcept
  {
    if (column < FileListColumn::Name)
    {
      return false;
    }

    const auto lastColumn = mProfile == FileListColumnProfile::Remote
                                ? FileListColumn::Owner
                                : FileListColumn::Modified;

    return column <= lastColumn;
  }

  void FileListCtrl::SortRows()
  {
    SortFileListRows(mRows, mSortColumn, mSortAscending, NaturalCompare);
  }

  void FileListCtrl::RefreshRows()
  {
    if (!mRows.empty())
    {
      RefreshItems(0, static_cast<long>(mRows.size() - 1U));
    }
    else
    {
      Refresh();
    }
  }

  void FileListCtrl::OnColumnClick(wxListEvent &event)
  {
    const int column = event.GetColumn();

    if (column < ColumnIndex(FileListColumn::Name) ||
        column > ColumnIndex(FileListColumn::Owner))
    {
      event.Skip();

      return;
    }

    const auto selectedColumn = static_cast<FileListColumn>(column);

    if (!HasColumn(selectedColumn))
    {
      event.Skip();

      return;
    }

    SortBy(selectedColumn, selectedColumn == mSortColumn ? !mSortAscending : true);

    event.Skip();
  }
} // namespace havremote::ui
