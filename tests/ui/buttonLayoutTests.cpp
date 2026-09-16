// SPDX-License-Identifier: MIT

#include "ui/buttonBitmap.hpp"
#include "ui/pickerLabel.hpp"
#include "ui/splitButton.hpp"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <wx/app.h>
#include <wx/aui/auibook.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/debug.h>
#include <wx/filepicker.h>
#include <wx/frame.h>
#include <wx/init.h>
#include <wx/log.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tooltip.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#include <wx/msw/wrapcctl.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
  using namespace havremote::ui;

  int toolkitAssertionCount{};

  class ButtonLayoutTestApp final : public wxApp
  {
  public:
    bool OnInit() override
    {
      SetExitOnFrameDelete(false);
      return true;
    }
  };

  void RecordToolkitAssertion(const wxString &file, const int line,
                              const wxString &function,
                              const wxString &condition,
                              const wxString &message)
  {
    ++toolkitAssertionCount;

    std::fprintf(stderr, "%s:%d: wx assertion in %s: %s (%s)\n",
                 file.utf8_str().data(), line, function.utf8_str().data(),
                 condition.utf8_str().data(), message.utf8_str().data());
  }

  wxBitmapBundle SolidBitmap(const unsigned char red,
                             const unsigned char green,
                             const unsigned char blue)
  {
    wxImage image{16, 16, false};
    image.SetRGB(wxRect{0, 0, 16, 16}, red, green, blue);

    return wxBitmapBundle::FromBitmap(wxBitmap{image});
  }

  void FitControl(wxFrame &frame, wxWindow &control)
  {
    auto *const sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(&control, 0, wxALIGN_CENTER_VERTICAL);

    frame.SetSizerAndFit(sizer);

    frame.Layout();

    control.Layout();
  }

  void CheckButtonSize(wxBitmapButton &button, const wxSize &bitmapSize)
  {
    const auto paddedSize = button.ClientToWindowSize(
        bitmapSize + button.FromDIP(wxSize{12, 12}));

    const auto minimum = button.GetMinSize();

    const auto nativeBest = button.GetBestSize();

    const auto actual = button.GetSize();

    CAPTURE(bitmapSize.x, bitmapSize.y, paddedSize.x, paddedSize.y,
            minimum.x, minimum.y, nativeBest.x, nativeBest.y,
            actual.x, actual.y);
    CHECK(minimum.x >= paddedSize.x);
    CHECK(minimum.y >= paddedSize.y);
    CHECK(minimum.x >= nativeBest.x);
    CHECK(minimum.y >= nativeBest.y);
    CHECK(actual.x >= minimum.x);
    CHECK(actual.y >= minimum.y);
  }

  void CheckButtonText(wxBitmapButton &button, const wxString &text)
  {
    CHECK(button.GetLabel() == text);
    CHECK(button.GetName() == text);
    REQUIRE(button.GetToolTip() != nullptr);
    CHECK(button.GetToolTip()->GetTip() == text);
  }

  void CheckTranslatedControlSize(wxControl &control, const wxString &label)
  {
    const auto best = control.GetBestSize();

    const auto actual = control.GetSize();

    const auto text = control.GetTextExtent(label);

    const auto client = control.GetClientSize();

    CAPTURE(label, best.x, best.y, actual.x, actual.y,
            text.x, text.y, client.x, client.y);
    CHECK(control.GetLabel() == label);
    CHECK(actual.x >= best.x);
    CHECK(actual.y >= best.y);
    CHECK(client.x > text.x);
    CHECK(client.y >= text.y);
  }

  void CheckTranslatedPicker(wxFileDirPickerCtrlBase &picker,
                             const wxString &label, const bool enabled)
  {
    auto *const control = picker.GetPickerCtrl();

    CheckTranslatedControlSize(*control, label);

    CHECK(control->IsEnabled() == enabled);

    for (auto *const child : control->GetChildren())
    {
      if (auto *const button = dynamic_cast<wxButton *>(child))
      {
        CheckTranslatedControlSize(*button, label);

        CHECK(button->IsEnabled() == enabled);
      }
    }
  }
} // namespace

wxIMPLEMENT_APP_NO_MAIN(ButtonLayoutTestApp);

int main(int argc, char **argv)
{
  // Keep toolkit diagnostics on stderr: an assertion or log dialog would block
  // unattended test runs. The test app never loads the production app/config.
  wxSetAssertHandler(RecordToolkitAssertion);

  delete wxLog::SetActiveTarget(new wxLogStderr);

  int toolkitArgc = 1;
  char *toolkitArgv[] = {argv[0], nullptr};

  if (!wxEntryStart(toolkitArgc, toolkitArgv))
  {
    std::fprintf(stderr, "Unable to initialize wxWidgets. A GUI display is required.\n");

    return 1;
  }

  if (!wxTheApp->CallOnInit())
  {
    wxEntryCleanup();

    return 1;
  }

  const auto result = Catch::Session{}.run(argc, argv);

  wxTheApp->OnExit();

  wxEntryCleanup();

  return toolkitAssertionCount == 0 ? result : 1;
}

TEST_CASE("icon buttons retain padding and the native minimum after layout",
          "[ui][bitmap-button][layout]")
{
  wxFrame frame{nullptr, wxID_ANY, "Button layout test"};

  const auto bitmap = SolidBitmap(220, 20, 60);

  auto *const button = new wxBitmapButton(&frame, wxID_ANY, bitmap);

  const auto nativeBest = button->GetBestSize();

  SetIconButtonBitmap(*button, bitmap);

  FitControl(frame, *button);

  CheckButtonSize(*button, bitmap.GetPreferredLogicalSizeFor(button));

  CHECK(button->GetMinSize().x >= nativeBest.x);
  CHECK(button->GetMinSize().y >= nativeBest.y);
}

TEST_CASE("icon button states and replacement bitmaps preserve padded bounds",
          "[ui][bitmap-button][layout]")
{
  wxFrame frame{nullptr, wxID_ANY, "Button state test"};

  const auto bitmap = SolidBitmap(220, 20, 60);

  auto *const button = new wxBitmapButton(&frame, wxID_ANY, bitmap);

  SetIconButtonBitmap(*button, bitmap);

  FitControl(frame, *button);

  const auto minimum = button->GetMinSize();
  const auto size = button->GetSize();

  for (const bool enabled : {false, true})
  {
    button->Enable(enabled);

    frame.Layout();

    CHECK(button->IsEnabled() == enabled);
    CHECK(button->GetMinSize() == minimum);
    CHECK(button->GetSize() == size);
    REQUIRE(button->GetBitmapDisabled().IsOk());
    CHECK(button->GetBitmapDisabled().GetLogicalSize() ==
          button->GetBitmap().GetLogicalSize());

    CheckButtonSize(*button, bitmap.GetPreferredLogicalSizeFor(button));
  }

  const auto replacement = SolidBitmap(20, 100, 240);

  SetIconButtonBitmap(*button, replacement);

  frame.Layout();

  CHECK(button->GetMinSize().x >= minimum.x);
  CHECK(button->GetMinSize().y >= minimum.y);
  CHECK(button->GetSize().x >= size.x);
  CHECK(button->GetSize().y >= size.y);

  CheckButtonSize(*button, replacement.GetPreferredLogicalSizeFor(button));

  const auto normalImage = button->GetBitmap().ConvertToImage();

  REQUIRE(normalImage.IsOk());
  CHECK(normalImage.GetBlue(0, 0) == 240);

  const auto disabledImage = button->GetBitmapDisabled().ConvertToImage();

  REQUIRE(disabledImage.IsOk());
  CHECK(disabledImage.GetRed(0, 0) == disabledImage.GetGreen(0, 0));
  CHECK(disabledImage.GetGreen(0, 0) == disabledImage.GetBlue(0, 0));
}

TEST_CASE("split buttons keep padded halves aligned and accessible",
          "[ui][bitmap-button][layout]")
{
  wxFrame frame{nullptr, wxID_ANY, "Split button layout test"};

  const auto bitmap = SolidBitmap(220, 20, 60);

  auto *const split = new SplitButton(&frame, wxID_ANY, bitmap,
                                      "Refresh", "Refresh options", {});

  FitControl(frame, *split);

  std::vector<wxBitmapButton *> buttons;

  for (auto *const child : split->GetChildren())
  {
    if (auto *const button = dynamic_cast<wxBitmapButton *>(child))
    {
      buttons.push_back(button);
    }
  }

  REQUIRE(buttons.size() == 2);

  auto &primary = *buttons[0];
  auto &dropdown = *buttons[1];

  CheckButtonSize(primary, bitmap.GetPreferredLogicalSizeFor(&primary));
  CheckButtonSize(dropdown, dropdown.GetBitmap().GetLogicalSize());

  CHECK(primary.GetMinSize().y == dropdown.GetMinSize().y);
  CHECK(primary.GetSize().y == dropdown.GetSize().y);
  CHECK(primary.GetPosition().y == dropdown.GetPosition().y);
  CHECK(dropdown.GetMinSize().x >= split->FromDIP(22));

  CheckButtonText(primary, "Refresh");
  CheckButtonText(dropdown, "Refresh options");

  split->SetButtonText("Reload", "Reload options");
  split->Layout();

  CheckButtonText(primary, "Reload");
  CheckButtonText(dropdown, "Reload options");
  CheckButtonSize(primary, bitmap.GetPreferredLogicalSizeFor(&primary));
  CheckButtonSize(dropdown, dropdown.GetBitmap().GetLogicalSize());

  CHECK(primary.GetSize().y == dropdown.GetSize().y);
}

TEST_CASE("Quick Connect fields and icon buttons share their row height",
          "[ui][bitmap-button][layout][quick-connect]")
{
  wxFrame frame{nullptr, wxID_ANY, "Quick Connect row test"};

  auto *const heading = new wxStaticText(&frame, wxID_ANY, "Quick Connect");
  auto *const protocol = new wxChoice(&frame, wxID_ANY, wxDefaultPosition,
                                      frame.FromDIP(wxSize{145, -1}));
  protocol->Append("FTP");
  protocol->Append("Explicit FTPS");
  protocol->Append("Implicit FTPS");
  protocol->Append("SFTP");
  protocol->SetSelection(3);

  auto *const host = new wxTextCtrl(&frame, wxID_ANY, "example.test",
                                    wxDefaultPosition,
                                    frame.FromDIP(wxSize{190, -1}));
  auto *const port = new wxSpinCtrl(&frame, wxID_ANY, {}, wxDefaultPosition,
                                    wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 22);
  auto *const username = new wxTextCtrl(&frame, wxID_ANY, "user",
                                        wxDefaultPosition,
                                        frame.FromDIP(wxSize{145, -1}));
  auto *const password = new wxTextCtrl(
      &frame, wxID_ANY, "secret", wxDefaultPosition,
      frame.FromDIP(wxSize{145, -1}), wxTE_PASSWORD | wxTE_PROCESS_ENTER);

  const auto bitmap = SolidBitmap(220, 20, 60);
  auto *const icon = new wxBitmapButton(&frame, wxID_ANY, bitmap);

  SetIconButtonBitmap(*icon, bitmap);

  auto *const split = new SplitButton(&frame, wxID_ANY, bitmap,
                                      "Site Manager", "Saved sites", {});
  auto *const logHeader = new wxPanel(&frame);
  auto *const logHeading = new wxStaticText(logHeader, wxID_ANY, "Message log");
  auto *const logClear = new wxBitmapButton(logHeader, wxID_ANY, bitmap);

  SetIconButtonBitmap(*logClear, bitmap);

  auto *const logRow = new wxBoxSizer(wxHORIZONTAL);
  logRow->Add(logHeading, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT,
              logHeader->FromDIP(5));
  logRow->Add(logClear, 0, wxALIGN_CENTER_VERTICAL);
  logHeader->SetSizer(logRow);

  const std::vector<wxWindow *> controls{
      protocol, host, port, username, password, icon, split};

#ifdef __WXMSW__
  const std::vector<wxWindow *> inputs{protocol, host, port, username, password};

  int compactHeight{};

  const auto applyCompactHeight = [&]
  {
    compactHeight = 0;

    for (auto *const input : inputs)
    {
      compactHeight = std::max(compactHeight, input->GetBestSize().y);
    }

    SetSquareIconButtonSize(*icon, compactHeight);
    SetSquareIconButtonSize(*logClear, compactHeight);

    split->SetButtonHeight(compactHeight);
  };

  applyCompactHeight();
#endif

  std::vector<wxSize> nativeMinimums;

  auto naturalHeight = heading->GetBestSize().y;

  for (auto *const control : controls)
  {
    const auto minimum = control->GetEffectiveMinSize();

    REQUIRE(minimum.x > 0);
    REQUIRE(minimum.y > 0);

    nativeMinimums.push_back(minimum);
    naturalHeight = std::max(naturalHeight, minimum.y);
  }

  std::vector<wxBitmapButton *> splitButtons;

  for (auto *const child : split->GetChildren())
  {
    if (auto *const button = dynamic_cast<wxBitmapButton *>(child))
    {
      splitButtons.push_back(button);
    }
  }

  REQUIRE(splitButtons.size() == 2);

  auto *const row = new wxBoxSizer(wxHORIZONTAL);
  row->Add(heading, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, frame.FromDIP(8));

  for (auto *const control : controls)
  {
    row->Add(control, control == host ? 1 : 0, wxEXPAND | wxRIGHT,
             control == split ? 0 : frame.FromDIP(5));
  }

  auto *const root = new wxBoxSizer(wxVERTICAL);
  root->Add(row, 0, wxEXPAND | wxALL, frame.FromDIP(8));
  root->Add(logHeader, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, frame.FromDIP(8));
  root->AddStretchSpacer();

  frame.SetSizerAndFit(root);

  const auto checkRow = [&]
  {
    frame.Layout();

    split->Layout();

    logHeader->Layout();

    const auto rowPosition = row->GetPosition();
    const auto rowSize = row->GetSize();

    CAPTURE(rowPosition.y, rowSize.x, rowSize.y);

    for (std::size_t index = 0; index < controls.size(); ++index)
    {
      const auto *const control = controls[index];
      const auto bounds = control->GetRect();

      INFO("control: " << control->GetName().ToStdString());
      CHECK(bounds.y == rowPosition.y);
      CHECK(bounds.height == rowSize.y);
      CHECK(bounds.width >= nativeMinimums[index].x);
      CHECK(bounds.height >= nativeMinimums[index].y);
    }

    for (auto *const button : splitButtons)
    {
      CHECK(button->GetPosition().y == 0);
      CHECK(button->GetSize().y == rowSize.y);

#ifndef __WXMSW__
      CheckButtonSize(*button, button->GetBitmap().GetLogicalSize());
#endif
    }

#ifndef __WXMSW__
    CheckButtonSize(*icon, bitmap.GetPreferredLogicalSizeFor(icon));
    CheckButtonSize(*logClear, bitmap.GetPreferredLogicalSizeFor(logClear));
#endif

    const auto logClientSize = logHeader->GetClientSize();
    const auto clearBounds = logClear->GetRect();

    CHECK(clearBounds.x + clearBounds.width == logClientSize.x);
    CHECK(std::abs(clearBounds.y - (logClientSize.y - clearBounds.height) / 2) <= 1);
    CHECK(std::abs(logHeading->GetPosition().y -
                   (logClientSize.y - logHeading->GetSize().y) / 2) <= 1);
    CHECK(logClear->IsEnabled() == icon->IsEnabled());

    const auto headingTop = rowPosition.y +
                            (rowSize.y - heading->GetSize().y) / 2;

    CHECK(std::abs(heading->GetPosition().y - headingTop) <= 1);

#ifdef __WXMSW__
    CHECK(rowSize.y == compactHeight);

    for (auto *const input : inputs)
    {
      CHECK(input->GetSize().y >= input->GetBestSize().y);
      CHECK(input->GetSize().y <= input->GetBestSize().y + frame.FromDIP(2));
    }

    for (auto *const button : {icon, logClear, splitButtons[0], splitButtons[1]})
    {
      const auto bitmapSize = button->FromPhys(button->GetBitmap().GetSize());

      CHECK(button->GetMinSize().y == compactHeight);
      CHECK(button->GetClientSize().x >= bitmapSize.x);
      CHECK(button->GetClientSize().y >= bitmapSize.y);
    }

    for (auto *const button : {icon, logClear, splitButtons[0]})
    {
      CHECK(button->GetMinSize() == wxSize{compactHeight, compactHeight});
      CHECK(button->GetSize() == wxSize{compactHeight, compactHeight});
    }

    CHECK(logClear->GetSize() == icon->GetSize());

    const auto *const dropdown = splitButtons[1];
    const auto dropdownBitmapSize = dropdown->ClientToWindowSize(
        dropdown->FromPhys(dropdown->GetBitmap().GetSize()));
    const auto dropdownWidth = std::max(
        std::min(dropdown->FromDIP(22), compactHeight), dropdownBitmapSize.x);

    CHECK(dropdown->GetMinSize().x == dropdownWidth);
    CHECK(dropdown->GetSize().x == dropdownWidth);
    CHECK(dropdown->GetSize().x <= std::max(rowSize.y, dropdownBitmapSize.x));
    CHECK(split->GetSize().x ==
          splitButtons[0]->GetSize().x + dropdown->GetSize().x);

    // wxChoice caches its requested height. Check the actual closed native
    // control too, so a cached value cannot conceal a shorter visible field.
    RECT nativeBounds{};

    REQUIRE(::GetWindowRect(static_cast<HWND>(protocol->GetHandle()),
                            &nativeBounds) != 0);

    const auto nativePosition = frame.ScreenToClient(
        wxPoint{static_cast<int>(nativeBounds.left),
                static_cast<int>(nativeBounds.top)});

    CHECK(nativePosition.y == rowPosition.y);
    CHECK(nativeBounds.bottom - nativeBounds.top == rowSize.y);

    const auto checkNativeTextMargins = [&](const wxWindow &field, HWND edit,
                                            const int textTop,
                                            const wxWindow &reference,
                                            HWND referenceEdit,
                                            const int referenceTop)
    {
      RECT clientBounds{};
      RECT referenceClientBounds{};

      REQUIRE(::GetClientRect(edit, &clientBounds) != 0);
      REQUIRE(::GetClientRect(referenceEdit, &referenceClientBounds) != 0);

      const auto clientHeight = clientBounds.bottom - clientBounds.top;
      const auto referenceClientHeight =
          referenceClientBounds.bottom - referenceClientBounds.top;
      const auto lineHeight = field.GetCharHeight();
      const auto referenceLineHeight = reference.GetCharHeight();
      const auto textBottom = clientHeight - textTop - lineHeight;
      const auto referenceBottom =
          referenceClientHeight - referenceTop - referenceLineHeight;

      INFO("text field: " << field.GetName().ToStdString());
      CAPTURE(textTop, textBottom, clientHeight, lineHeight,
              referenceTop, referenceBottom, referenceClientHeight,
              referenceLineHeight);
      CHECK(textTop >= 0);
      CHECK(textBottom >= 0);
      CHECK(lineHeight == referenceLineHeight);

      // A font's line box is not its visible ink. Preserve native text margins
      // measured at this font's preferred control height. A stretched field
      // must not gain blank space below the line. Allow only pixel rounding.

      CHECK(std::abs(textTop - referenceTop) <= 1);
      CHECK(std::abs(textBottom - referenceBottom) <= 1);
    };

    for (auto *const text : {host, username, password})
    {
      wxTextCtrl reference{&frame, wxID_ANY, text->GetValue(),
                           wxDefaultPosition, wxDefaultSize,
                           text->GetWindowStyleFlag()};
      reference.Hide();
      reference.SetFont(text->GetFont());
      reference.Enable(text->IsEnabled());
      reference.SetSize(reference.GetBestSize());

      const auto origin = text->PositionToCoords(0);
      const auto referenceOrigin = reference.PositionToCoords(0);

      REQUIRE(origin != wxDefaultPosition);
      REQUIRE(referenceOrigin != wxDefaultPosition);

      checkNativeTextMargins(*text, static_cast<HWND>(text->GetHandle()),
                             origin.y, reference,
                             static_cast<HWND>(reference.GetHandle()),
                             referenceOrigin.y);
    }

    const auto spinEditHandle = [](const wxSpinCtrl &spin) -> HWND
    {
      const auto control = static_cast<HWND>(spin.GetHandle());

      const auto edit = reinterpret_cast<HWND>(
          ::SendMessage(control, UDM_GETBUDDY, 0, 0));

      REQUIRE(edit != nullptr);
      REQUIRE(edit != control);

      wchar_t nativeClass[32]{};

      REQUIRE(::GetClassNameW(edit, nativeClass, 32) > 0);
      REQUIRE(wxString{nativeClass}.CmpNoCase("EDIT") == 0);

      return edit;
    };

    wxSpinCtrl referencePort{&frame, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, port->GetMin(), port->GetMax(), port->GetValue()};
    referencePort.Hide();
    referencePort.SetFont(port->GetFont());
    referencePort.Enable(port->IsEnabled());
    referencePort.SetSize(referencePort.GetBestSize());

    const auto spinEdit = spinEditHandle(*port);
    const auto referenceSpinEdit = spinEditHandle(referencePort);
    const auto spinOrigin = ::SendMessage(spinEdit, EM_POSFROMCHAR, 0, 0);
    const auto referenceSpinOrigin =
        ::SendMessage(referenceSpinEdit, EM_POSFROMCHAR, 0, 0);

    REQUIRE(spinOrigin != -1);
    REQUIRE(referenceSpinOrigin != -1);

    checkNativeTextMargins(*port, spinEdit,
                           static_cast<short>(HIWORD(spinOrigin)),
                           referencePort, referenceSpinEdit,
                           static_cast<short>(HIWORD(referenceSpinOrigin)));

    CHECK(::SendMessage(static_cast<HWND>(password->GetHandle()),
                        EM_GETPASSWORDCHAR, 0, 0) != 0);
#endif

    CHECK(password->HasFlag(wxTE_PASSWORD));
    CHECK(password->HasFlag(wxTE_PROCESS_ENTER));
    CHECK(password->GetValue() == "secret");
    CHECK(port->GetValue() == 22);
  };

  checkRow();

  CHECK(row->GetSize().y == naturalHeight);

  const auto initialClientSize = frame.GetClientSize();
  const auto initialRowSize = row->GetSize();
  const auto initialHostWidth = host->GetSize().x;
  const auto extraWidth = frame.FromDIP(240);

  frame.SetClientSize(initialClientSize + wxSize{extraWidth, frame.FromDIP(80)});

  checkRow();

  CHECK(row->GetSize() == initialRowSize + wxSize{extraWidth, 0});
  CHECK(host->GetSize().x == initialHostWidth + extraWidth);

  frame.SetClientSize(initialClientSize);

  checkRow();

  CHECK(row->GetSize() == initialRowSize);
  CHECK(host->GetSize().x == initialHostWidth);

#ifndef __WXMSW__
  // GTK keeps its padded controls and can expand the entire row vertically
  const auto extraHeight = frame.FromDIP(8);

  row->SetMinSize(wxSize{-1, naturalHeight + extraHeight});
  frame.SetClientSize(initialClientSize + wxSize{0, extraHeight});

  checkRow();

  CHECK(row->GetSize().y == naturalHeight + extraHeight);
#endif

  const auto stateRowSize = row->GetSize();

  for (const bool enabled : {false, true})
  {
    for (auto *const control : controls)
    {
      control->Enable(enabled);
      CHECK(control->IsEnabled() == enabled);
    }

    logClear->Enable(enabled);

    CHECK(logClear->IsEnabled() == enabled);

    checkRow();

    CHECK(row->GetSize() == stateRowSize);
  }

  SetIconButtonBitmap(*icon, SolidBitmap(20, 100, 240));
  SetIconButtonBitmap(*logClear, SolidBitmap(20, 100, 240));

#ifdef __WXMSW__
  applyCompactHeight();
#endif

  checkRow();

  CHECK(row->GetSize() == stateRowSize);

#ifdef __WXMSW__
  std::vector<wxFont> originalFonts;

  for (auto *const input : inputs)
  {
    originalFonts.push_back(input->GetFont());
    input->SetFont(input->GetFont().Scaled(1.5f));
  }

  applyCompactHeight();

  frame.SetClientSize(initialClientSize + frame.FromDIP(wxSize{240, 80}));

  checkRow();

  CHECK(row->GetSize().y > naturalHeight);

  for (std::size_t index = 0; index < inputs.size(); ++index)
  {
    inputs[index]->SetFont(originalFonts[index]);
  }

  applyCompactHeight();

  frame.SetClientSize(initialClientSize);

  checkRow();

  CHECK(row->GetSize() == initialRowSize);

  // Repeated state refreshes must not accumulate the bitmap helper's padding
  for (int refresh = 0; refresh < 3; ++refresh)
  {
    SetIconButtonBitmap(*icon, bitmap);
    SetIconButtonBitmap(*logClear, bitmap);

    applyCompactHeight();

    checkRow();

    CHECK(row->GetSize() == initialRowSize);
  }

  int enterCount{};

  password->Bind(wxEVT_TEXT_ENTER, [&](wxCommandEvent &event)
                 {
    ++enterCount;

    CHECK(event.GetString() == "secret"); });

  ::SendMessage(static_cast<HWND>(password->GetHandle()), WM_CHAR, VK_RETURN, 0);

  CHECK(enterCount == 1);
  CHECK(password->GetValue() == "secret");
#endif

  CheckButtonText(*splitButtons[0], "Site Manager");
  CheckButtonText(*splitButtons[1], "Saved sites");
}

TEST_CASE("connection page buttons fit translated labels without resizing the frame",
          "[ui][text-button][layout][translation]")
{
  const std::array<const char *, 10> english{
      "New folder", "Edit", "Rename", "Delete", "New folder", "New file",
      "Edit", "Rename", "Permissions...", "Delete permanently"};

  const std::array<const char *, 10> german{
      "Neuer Ordner", "Bearbeiten", "Umbenennen", "Löschen", "Neuer Ordner",
      "Neue Datei", "Bearbeiten", "Umbenennen", "Berechtigungen...",
      "Endgültig löschen"};

  struct ConnectionPage
  {
    wxPanel *panel{};
    wxDirPickerCtrl *directory{};
    std::array<wxButton *, 10> buttons{};
  };

  for (const bool startInGerman : {false, true})
  {
    CAPTURE(startInGerman);

    wxFrame frame{nullptr, wxID_ANY, "Translated connection pages"};

    auto *const notebook = new wxAuiNotebook(&frame, wxID_ANY);

    std::array<ConnectionPage, 2> pages{};

    const auto setLabels = [&](ConnectionPage &page, const bool useGerman)
    {
      const auto &labels = useGerman ? german : english;

      for (std::size_t index = 0; index < page.buttons.size(); ++index)
      {
        page.buttons[index]->SetLabel(wxString::FromUTF8(labels[index]));
      }

      SetPickerButtonLabel(*page.directory,
                           useGerman ? "Durchsuchen..." : "Browse...");
    };

    for (auto &page : pages)
    {
      page.panel = new wxPanel(notebook);

      auto *const panes = new wxBoxSizer(wxHORIZONTAL);

      for (std::size_t paneIndex = 0; paneIndex < 2; ++paneIndex)
      {
        auto *const pane = new wxPanel(page.panel);
        auto *const content = new wxBoxSizer(wxVERTICAL);

        if (paneIndex == 0)
        {
          page.directory = new wxDirPickerCtrl(
              pane, wxID_ANY, {}, "Choose directory", wxDefaultPosition,
              wxDefaultSize, wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);

          content->Add(page.directory, 0, wxEXPAND);
        }

        content->AddStretchSpacer();

        auto *const buttons = new wxBoxSizer(wxHORIZONTAL);

        const auto begin = paneIndex == 0 ? 0U : 4U;
        const auto end = paneIndex == 0 ? 4U : 10U;

        for (auto index = begin; index < end; ++index)
        {
          page.buttons[index] = new wxButton(pane, wxID_ANY, english[index]);
          buttons->Add(page.buttons[index], 0, wxRIGHT, pane->FromDIP(4));
        }

        content->Add(buttons, 0, wxEXPAND);

        pane->SetSizer(content);

        panes->Add(pane, paneIndex == 0 ? 2 : 3, wxEXPAND | wxALL,
                   page.panel->FromDIP(8));
      }

      page.panel->SetSizer(panes);

      setLabels(page, startInGerman);

      notebook->AddPage(page.panel, "Connection", pages[0].panel == page.panel);
    }

    auto *const root = new wxBoxSizer(wxVERTICAL);
    root->Add(notebook, 1, wxEXPAND);

    frame.SetSizerAndFit(root);

    auto preferred = frame.GetClientSize();

    for (const bool useGerman : {false, true})
    {
      for (auto &page : pages)
      {
        setLabels(page, useGerman);
      }

      preferred.IncTo(root->GetMinSize());
    }

    for (auto &page : pages)
    {
      setLabels(page, startInGerman);
    }

    frame.SetClientSize(preferred + frame.FromDIP(wxSize{240, 120}));

    for (std::size_t index = 0; index < pages.size(); ++index)
    {
      notebook->SetSelection(index);

      frame.Layout();

      pages[index].panel->Layout();
    }

    notebook->SetSelection(0);

    frame.Layout();

    const auto frameBounds = frame.GetRect();
    const auto pageSize = pages[0].panel->GetSize();

    const auto checkPages = [&](const bool useGerman, const bool enabled)
    {
      CHECK(frame.GetRect() == frameBounds);
      CHECK(pages[0].panel->GetSize() == pageSize);

      const auto &labels = useGerman ? german : english;

      for (auto &page : pages)
      {
        for (std::size_t index = 0; index < page.buttons.size(); ++index)
        {
          CheckTranslatedControlSize(*page.buttons[index],
                                     wxString::FromUTF8(labels[index]));

          CHECK(page.buttons[index]->IsEnabled() == enabled);
        }

        CheckTranslatedPicker(*page.directory,
                              useGerman ? "Durchsuchen..." : "Browse...",
                              enabled);
      }
    };

    checkPages(startInGerman, true);

    for (const bool enabled : {false, true})
    {
      for (const bool useGerman : {true, false, true, false})
      {
        for (auto &page : pages)
        {
          setLabels(page, useGerman);

          page.directory->Enable(enabled);

          for (auto *const button : page.buttons)
          {
            button->Enable(enabled);
          }
        }

        frame.Layout();

        for (auto &page : pages)
        {
          page.panel->Layout();
        }

        CHECK_FALSE(pages[1].panel->IsShown());

        checkPages(useGerman, enabled);
      }
    }

    notebook->SetSelection(1);

    frame.Layout();

    CHECK(pages[1].panel->IsShown());

    checkPages(false, true);
  }
}

TEST_CASE("file picker Browse labels fit English and German in settings rows",
          "[ui][file-picker][layout][translation]")
{
  for (const bool startInGerman : {false, true})
  {
    wxFrame frame{nullptr, wxID_ANY, "Translated file picker"};

    auto *const picker = new wxFilePickerCtrl(
        &frame, wxID_ANY, {}, "Choose application", wxFileSelectorDefaultWildcardStr,
        wxDefaultPosition, wxDefaultSize,
        wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);

    SetPickerButtonLabel(*picker, "Durchsuchen...");
    FitControl(frame, *picker);

    const auto frameBounds = frame.GetRect();

    for (const bool enabled : {false, true})
    {
      for (const bool useGerman : {startInGerman, !startInGerman, startInGerman})
      {
        const wxString label = useGerman ? "Durchsuchen..." : "Browse...";

        SetPickerButtonLabel(*picker, label);

        picker->Enable(enabled);

        frame.Layout();

        CheckTranslatedPicker(*picker, label, enabled);

        CHECK(frame.GetRect() == frameBounds);
      }
    }
  }
}
