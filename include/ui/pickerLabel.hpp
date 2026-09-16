// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_PICKER_LABEL_HPP
#define HAVREMOTE_INCLUDE_UI_PICKER_LABEL_HPP

#include <wx/button.h>
#include <wx/filepicker.h>

namespace havremote::ui
{
  inline void SetPickerButtonLabel(wxFileDirPickerCtrlBase &picker,
                                   const wxString &label)
  {
    auto *const control = picker.GetPickerCtrl();
    control->SetLabel(label);

    // GTK wraps the Browse button in a control that does not forward SetLabel()
    for (auto *child : control->GetChildren())
    {
      if (auto *button = dynamic_cast<wxButton *>(child))
      {
        button->SetLabel(label);
        button->SetInitialSize(wxDefaultSize);
      }
    }
    control->InvalidateBestSize();
    picker.Layout();
  }
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_PICKER_LABEL_HPP
