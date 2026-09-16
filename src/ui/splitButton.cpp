// SPDX-License-Identifier: MIT

#include "ui/splitButton.hpp"

#include "ui/buttonBitmap.hpp"

#include <wx/bmpbuttn.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/settings.h>

#include <algorithm>
#include <format>
#include <utility>

namespace havremote::ui
{
  namespace
  {
    wxBitmapBundle DropdownArrowBitmap()
    {
      const auto colour =
          wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);

      const auto svg = std::format(
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" "
          "height=\"5\" viewBox=\"0 0 8 5\">"
          "<path fill=\"#{:02X}{:02X}{:02X}\" d=\"M0 0h8L4 5z\"/>"
          "</svg>",
          static_cast<unsigned>(colour.Red()),
          static_cast<unsigned>(colour.Green()),
          static_cast<unsigned>(colour.Blue()));

      return wxBitmapBundle::FromSVG(svg.c_str(), wxSize{8, 5});
    }
  } // namespace

  SplitButton::SplitButton(wxWindow *const parent,
                           const wxWindowID id,
                           const wxBitmapBundle &bitmap,
                           const wxString &primaryText,
                           const wxString &dropdownText,
                           MenuBuilder menuBuilder)
      : wxPanel(parent, id),
        mMenuBuilder(std::move(menuBuilder))
  {
    mPrimaryButton = new wxBitmapButton(this, wxID_ANY, bitmap);

    SetIconButtonBitmap(*mPrimaryButton, bitmap);

    const auto dropdownBitmap = DropdownArrowBitmap();

    mDropdownButton = new wxBitmapButton(this, wxID_ANY, dropdownBitmap);

    SetIconButtonBitmap(*mDropdownButton, dropdownBitmap);

    auto primarySize = mPrimaryButton->GetBestSize();
    auto dropdownSize = mDropdownButton->GetBestSize();

    const auto height = std::max(primarySize.y, dropdownSize.y);
    primarySize.y = height;

    dropdownSize.x = std::max(dropdownSize.x, FromDIP(22));
    dropdownSize.y = height;

    mPrimaryButton->SetMinSize(primarySize);

    mDropdownButton->SetMinSize(dropdownSize);

    auto *const sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(mPrimaryButton, 0, wxEXPAND);
    sizer->Add(mDropdownButton, 0, wxEXPAND);

    SetSizer(sizer);
    SetButtonText(primaryText, dropdownText);

    mPrimaryButton->Bind(wxEVT_BUTTON, &SplitButton::OnPrimary, this);
    mDropdownButton->Bind(wxEVT_BUTTON, &SplitButton::OnDropdown, this);
    mPrimaryButton->Bind(wxEVT_KEY_DOWN, &SplitButton::OnKeyDown, this);
    mDropdownButton->Bind(wxEVT_KEY_DOWN, &SplitButton::OnKeyDown, this);
  }

  void SplitButton::SetButtonText(const wxString &primaryText,
                                  const wxString &dropdownText)
  {
    mPrimaryButton->SetLabel(primaryText);
    mPrimaryButton->SetName(primaryText);
    mPrimaryButton->SetToolTip(primaryText);
    mDropdownButton->SetLabel(dropdownText);
    mDropdownButton->SetName(dropdownText);
    mDropdownButton->SetToolTip(dropdownText);
  }

  void SplitButton::SetButtonHeight(const int height)
  {
    SetSquareIconButtonSize(*mPrimaryButton, height);
    SetSquareIconButtonSize(*mDropdownButton, height);

    const auto commonHeight = std::max(mPrimaryButton->GetMinSize().y,
                                       mDropdownButton->GetMinSize().y);

    SetSquareIconButtonSize(*mPrimaryButton, commonHeight);

    wxSize dropdownSize{std::min(commonHeight, FromDIP(22)), commonHeight};

    const auto dropdownBitmap = mDropdownButton->GetBitmap();

    if (dropdownBitmap.IsOk())
    {
      dropdownSize.x = std::max(
          dropdownSize.x, mDropdownButton->ClientToWindowSize(
                                             mDropdownButton->FromPhys(dropdownBitmap.GetSize()))
                              .x);
    }

    mDropdownButton->SetMinSize(dropdownSize);

    SetMinSize(wxSize{GetMinSize().x, commonHeight});
  }

  void SplitButton::SendPrimaryAction()
  {
    wxCommandEvent event(wxEVT_BUTTON, GetId());

    event.SetEventObject(this);

    HandleWindowEvent(event);
  }

  void SplitButton::ShowMenu()
  {
    wxMenu menu;

    if (mMenuBuilder)
    {
      mMenuBuilder(menu);
    }

    if (menu.GetMenuItemCount() != 0)
    {
      PopupMenu(&menu, 0, GetClientSize().y);
    }
  }

  void SplitButton::OnPrimary(wxCommandEvent &)
  {
    SendPrimaryAction();
  }

  void SplitButton::OnDropdown(wxCommandEvent &)
  {
    ShowMenu();
  }

  void SplitButton::OnKeyDown(wxKeyEvent &event)
  {
    const auto key = event.GetKeyCode();

    if (key == WXK_F4 || key == WXK_DOWN)
    {
      ShowMenu();

      return;
    }

    event.Skip();
  }
} // namespace havremote::ui
