#pragma once
#include <wx/string.h>

namespace wxHelper
{
    inline fs::path MakeFSPath(const wxString& str)
    {
        auto tmpUtf8 = str.ToUTF8();
        auto sv = std::basic_string_view<char8_t>((const char8_t*)tmpUtf8.data(), tmpUtf8.length());
        return fs::path(sv);
    }

	inline wxString FromUtf8(std::string_view str)
	{
		return wxString::FromUTF8(str.data(), str.size());
	}

	inline wxString FromPath(const fs::path& path)
	{
		std::string str = _pathToUtf8(path);
		return wxString::FromUTF8(str.data(), str.size());
	}

	inline bool IsDarkColour(const wxColour& c)
	{
		const uint32 lightness = (c.GetRed() + c.GetGreen() + c.GetBlue()) / 3;
		return lightness < 128;
	}

	inline wxColour CalculateAccentColour(const wxColour& bgColour)
	{
		wxColour bgColourSecondary;
		const uint32 bgLightness = (bgColour.GetRed() + bgColour.GetGreen() + bgColour.GetBlue()) / 3;
		const bool isDarkTheme = bgLightness < 128;
		if (isDarkTheme)
		{
			bgColourSecondary = wxColour(
				std::min(255, bgColour.Red() + 10),
				std::min(255, bgColour.Green() + 10),
				std::min(255, bgColour.Blue() + 14));
			return bgColourSecondary;
		}
		bgColourSecondary = bgColour.ChangeLightness(90); // color for even rows
		// for very light themes we'll use a blue tint to match the older Windows Cemu look
		if (bgLightness > 250)
			bgColourSecondary = wxColour(bgColour.Red() - 13, bgColour.Green() - 6, bgColour.Blue() - 2);
		return bgColourSecondary;
	}

};
