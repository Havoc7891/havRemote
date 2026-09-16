// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_UPDATE_DIALOG_HPP
#define HAVREMOTE_INCLUDE_UI_UPDATE_DIALOG_HPP

#include <string_view>

class wxWindow;

namespace havremote::localization
{
  class TranslationCatalog;
}

namespace havremote::updates
{
  struct Release;
}

namespace havremote::ui
{
  enum class UpdateDialogAction
  {
    Download,
    ViewRelease,
    SkipVersion,
    Later,
  };

  [[nodiscard]] UpdateDialogAction ShowUpdateAvailableDialog(
      wxWindow *parent,
      const localization::TranslationCatalog &translations,
      std::string_view installedVersion,
      const updates::Release &release);
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_UPDATE_DIALOG_HPP
