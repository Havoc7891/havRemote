// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_REMOTE_PERMISSIONS_DIALOG_HPP
#define HAVREMOTE_INCLUDE_UI_REMOTE_PERMISSIONS_DIALOG_HPP

#include <wx/dialog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

class wxButton;
class wxCheckBox;
class wxCommandEvent;
class wxTextCtrl;

namespace havremote::localization
{
  class TranslationCatalog;
}

namespace havremote::ui
{
  // Mixed or unavailable permissions start without a value. The user must
  // enter an exact POSIX mode before accepting the dialog.
  class RemotePermissionsDialog final : public wxDialog
  {
  public:
    RemotePermissionsDialog(
        wxWindow *parent,
        std::size_t selectedItemCount,
        std::optional<std::uint32_t> initialPermissions,
        const localization::TranslationCatalog &translations);

    [[nodiscard]] std::uint32_t Permissions() const noexcept;

  private:
    void OnOctalChanged(wxCommandEvent &event);
    void OnPermissionBitChanged(wxCommandEvent &event);
    void UpdateFromOctal();
    void UpdateOctalFromBits();

    wxTextCtrl *mOctal{};
    std::array<wxCheckBox *, 12> mPermissionBits{};
    wxButton *mOkButton{};
    std::optional<std::uint32_t> mPermissions;
    bool mSynchronizing{};
  };
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_REMOTE_PERMISSIONS_DIALOG_HPP
