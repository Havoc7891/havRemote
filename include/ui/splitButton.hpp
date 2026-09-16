// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_SPLIT_BUTTON_HPP
#define HAVREMOTE_INCLUDE_UI_SPLIT_BUTTON_HPP

#include <wx/bmpbndl.h>
#include <wx/panel.h>

#include <functional>

class wxBitmapButton;
class wxMenu;

namespace havremote::ui
{
  class SplitButton final : public wxPanel
  {
  public:
    using MenuBuilder = std::function<void(wxMenu &)>;

    SplitButton(wxWindow *parent,
                wxWindowID id,
                const wxBitmapBundle &bitmap,
                const wxString &primaryText,
                const wxString &dropdownText,
                MenuBuilder menuBuilder);

    void SetButtonText(const wxString &primaryText,
                       const wxString &dropdownText);
    void SetButtonHeight(int height);

  private:
    void SendPrimaryAction();
    void ShowMenu();
    void OnPrimary(wxCommandEvent &event);
    void OnDropdown(wxCommandEvent &event);
    void OnKeyDown(wxKeyEvent &event);

    wxBitmapButton *mPrimaryButton{};
    wxBitmapButton *mDropdownButton{};
    MenuBuilder mMenuBuilder;
  };
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_SPLIT_BUTTON_HPP
