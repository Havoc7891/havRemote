// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_FILE_LIST_CTRL_HPP
#define HAVREMOTE_INCLUDE_UI_FILE_LIST_CTRL_HPP

#include "ui/fileListModel.hpp"

#include <wx/bmpbndl.h>
#include <wx/event.h>
#include <wx/listctrl.h>
#include <wx/string.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace havremote::ui
{
  [[nodiscard]] inline bool IsFileListDeleteKey(const wxKeyEvent &event)
  {
    return event.GetEventType() == wxEVT_KEY_DOWN &&
           (event.GetKeyCode() == WXK_DELETE ||
            event.GetKeyCode() == WXK_NUMPAD_DELETE) &&
           event.GetModifiers() == wxMOD_NONE && !event.IsAutoRepeat();
  }

  struct FileListColumnLabels
  {
    wxString name;
    wxString size;
    wxString type;
    wxString modified;
    wxString permissions;
    wxString owner;
  };

  struct FileListIcons
  {
    wxBitmapBundle folder;
    wxBitmapBundle file;
    wxBitmapBundle symlink;
    wxBitmapBundle other;
  };

  class FileListCtrl final : public wxListCtrl
  {
  public:
    explicit FileListCtrl(wxWindow *parent,
                          wxWindowID id = wxID_ANY,
                          FileListColumnProfile profile =
                              FileListColumnProfile::Local,
                          const wxPoint &position = wxDefaultPosition,
                          const wxSize &size = wxDefaultSize,
                          long style = 0);

    void SetColumnLabels(const FileListColumnLabels &labels);
    void SetIcons(const FileListIcons &icons);
    void SetRows(std::vector<FileListRow> rows);
    void ClearRows();
    void SortBy(FileListColumn column, bool ascending);
    [[nodiscard]] FileListColumnWidths ColumnWidthsDips() const;
    void ApplyColumnWidthsDips(
        std::span<const FileListColumnWidth> widths);

    [[nodiscard]] const std::vector<FileListRow> &Rows() const noexcept;
    [[nodiscard]] const FileListRow *RowAt(long visualIndex) const noexcept;
    [[nodiscard]] std::optional<std::size_t> SourceIndexAt(
        long visualIndex) const noexcept;
    [[nodiscard]] std::vector<std::size_t> SelectedSourceIndices() const;
    [[nodiscard]] std::optional<FileListRow> SingleSelectedRow() const;
    [[nodiscard]] std::size_t SelectedRowCount() const noexcept;
    [[nodiscard]] FileListColumn SortColumn() const noexcept;
    [[nodiscard]] bool IsSortAscending() const noexcept;

  protected:
    [[nodiscard]] wxString OnGetItemText(long item,
                                         long column) const override;
    [[nodiscard]] int OnGetItemImage(long item) const override;

  private:
    struct ViewState;

    [[nodiscard]] ViewState CaptureViewState() const;
    void RestoreViewState(const ViewState &state);
    [[nodiscard]] bool HasColumn(FileListColumn column) const noexcept;
    void SortRows();
    void RefreshRows();
    void OnColumnClick(wxListEvent &event);

    std::vector<FileListRow> mRows;
    FileListColumnProfile mProfile{FileListColumnProfile::Local};
    FileListColumn mSortColumn{FileListColumn::Name};
    bool mSortAscending{true};
    int mFolderImage{-1};
    int mFileImage{-1};
    int mSymlinkImage{-1};
    int mOtherImage{-1};
  };
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_FILE_LIST_CTRL_HPP
