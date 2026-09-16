// SPDX-License-Identifier: MIT

#ifndef HAVREMOTE_INCLUDE_UI_BUTTON_BITMAP_HPP
#define HAVREMOTE_INCLUDE_UI_BUTTON_BITMAP_HPP

#include <wx/anybutton.h>
#include <wx/bmpbndl.h>
#include <wx/image.h>

#include <algorithm>
#include <utility>

namespace havremote::ui
{
  namespace detail
  {
    class GreyscaleBitmapBundleImpl final : public wxBitmapBundleImpl
    {
    public:
      explicit GreyscaleBitmapBundleImpl(wxBitmapBundle source)
          : mSource(std::move(source)) {}

      wxSize GetDefaultSize() const override
      {
        return mSource.GetDefaultSize();
      }

      wxSize GetPreferredBitmapSizeAtScale(const double scale) const override
      {
        return mSource.GetPreferredBitmapSizeAtScale(scale);
      }

      wxBitmap GetBitmap(const wxSize &size) override
      {
        const auto normal = mSource.GetBitmap(size);
        if (!normal.IsOk())
        {
          return {};
        }

        const auto greyscale = normal.ConvertToImage().ConvertToGreyscale();
        if (!greyscale.IsOk())
        {
          return {};
        }

        // wxMSW ignores the scale argument of wxBitmap(wxImage, ...), so set
        // it explicitly to preserve the source bundle's logical dimensions.
        wxBitmap disabled{greyscale};
        disabled.SetScaleFactor(normal.GetScaleFactor());
        return disabled;
      }

    private:
      wxBitmapBundle mSource;
    };

    inline wxBitmapBundle MakeGreyscaleBitmapBundle(
        const wxBitmapBundle &normalBitmap)
    {
      return normalBitmap.IsOk()
                 ? wxBitmapBundle::FromImpl(
                       new GreyscaleBitmapBundleImpl(normalBitmap))
                 : wxBitmapBundle{};
    }
  } // namespace detail

  inline void SetIconButtonBitmap(
      wxAnyButton &button,
      const wxBitmapBundle &normalBitmap)
  {
    button.SetBitmap(normalBitmap);

    // ConvertToGreyscale retains the PNG alpha channel. The derived bundle
    // converts each requested raster size lazily, preserving DPI behaviour.
    button.SetBitmapDisabled(
        detail::MakeGreyscaleBitmapBundle(normalBitmap));

    if (normalBitmap.IsOk())
    {
      // Exact-fit bitmap buttons can have no native padding. Keep space
      // around the icon without shrinking a larger native minimum size.
      auto minimumSize = button.GetBestSize();
      minimumSize.IncTo(button.ClientToWindowSize(
          normalBitmap.GetPreferredLogicalSizeFor(&button) +
          button.FromDIP(wxSize{12, 12})));
      button.SetMinSize(minimumSize);
    }
  }

  inline void SetSquareIconButtonSize(wxAnyButton &button, const int side)
  {
    auto extent = side;
    const auto bitmap = button.GetBitmap();
    if (bitmap.IsOk())
    {
      const auto bitmapSize =
          button.ClientToWindowSize(button.FromPhys(bitmap.GetSize()));
      extent = std::max({extent, bitmapSize.x, bitmapSize.y});
    }
    button.SetMinSize(wxSize{extent, extent});
  }
} // namespace havremote::ui

#endif // HAVREMOTE_INCLUDE_UI_BUTTON_BITMAP_HPP
