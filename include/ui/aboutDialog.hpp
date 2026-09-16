// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_ABOUT_DIALOG_HPP
#define HAVREMOTE_INCLUDE_UI_ABOUT_DIALOG_HPP

class wxWindow;

namespace havremote::localization
{
  class TranslationCatalog;
}

namespace havremote::ui
{
  void ShowAboutDialog(
      wxWindow *parent,
      const localization::TranslationCatalog &translations);
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_ABOUT_DIALOG_HPP
