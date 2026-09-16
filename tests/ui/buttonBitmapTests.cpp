// SPDX-License-Identifier: MIT

#include "ui/buttonBitmap.hpp"

#include <catch2/catch_test_macros.hpp>

#include <wx/bitmap.h>
#include <wx/bmpbndl.h>
#include <wx/image.h>

using namespace havremote::ui;

TEST_CASE("disabled button bitmaps are grayscale and retain alpha",
          "[ui][bitmap-button]")
{
  wxImage sourceImage{2, 1, false};
  sourceImage.SetRGB(0, 0, 255, 0, 0);
  sourceImage.SetRGB(1, 0, 0, 0, 255);
  sourceImage.InitAlpha();
  sourceImage.SetAlpha(0, 0, 64);
  sourceImage.SetAlpha(1, 0, 192);

  const auto sourceBundle =
      wxBitmapBundle::FromBitmap(wxBitmap{sourceImage});

  auto disabledBundle =
      detail::MakeGreyscaleBitmapBundle(sourceBundle);

  REQUIRE(disabledBundle.IsOk());
  CHECK(disabledBundle.GetDefaultSize() == sourceBundle.GetDefaultSize());

  const auto disabledBitmap = disabledBundle.GetBitmap({2, 1});

  REQUIRE(disabledBitmap.IsOk());

  const auto disabledImage = disabledBitmap.ConvertToImage();

  REQUIRE(disabledImage.IsOk());
  REQUIRE(disabledImage.HasAlpha());

  for (int x = 0; x < 2; ++x)
  {
    CHECK(disabledImage.GetRed(x, 0) == disabledImage.GetGreen(x, 0));
    CHECK(disabledImage.GetGreen(x, 0) == disabledImage.GetBlue(x, 0));
  }

  CHECK(disabledImage.GetAlpha(0, 0) == 64);
  CHECK(disabledImage.GetAlpha(1, 0) == 192);
}

TEST_CASE("disabled button bitmap bundles retain DPI dimensions",
          "[ui][bitmap-button]")
{
  wxImage sourceImage{32, 32, false};
  sourceImage.SetRGB(wxRect{0, 0, 32, 32}, 220, 20, 60);

  wxBitmap sourceBitmap{sourceImage};
  sourceBitmap.SetScaleFactor(2.0);

  auto sourceBundle = wxBitmapBundle::FromBitmap(sourceBitmap);

  auto disabledBundle =
      detail::MakeGreyscaleBitmapBundle(sourceBundle);

  REQUIRE(disabledBundle.IsOk());
  CHECK(disabledBundle.GetDefaultSize() == sourceBundle.GetDefaultSize());

  for (const auto size : {wxSize{16, 16}, wxSize{32, 32}})
  {
    const auto normal = sourceBundle.GetBitmap(size);
    const auto disabled = disabledBundle.GetBitmap(size);

    REQUIRE(normal.IsOk());
    REQUIRE(disabled.IsOk());
    CHECK(disabled.GetSize() == normal.GetSize());
    CHECK(disabled.GetLogicalSize() == normal.GetLogicalSize());
    CHECK(disabled.GetScaleFactor() == normal.GetScaleFactor());
  }
}
