#include "gui/components/wxInputDraw.h"
#include "gui/wxHelper.h"
#include "gui/CemuApp.h"

#include <wx/dcbuffer.h>

wxInputDraw::wxInputDraw(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size)
	: wxWindow(parent, id, pos, size, 0, wxPanelNameStr)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Bind(wxEVT_PAINT, &wxInputDraw::OnPaintEvent, this);
}

wxInputDraw::~wxInputDraw() { Unbind(wxEVT_PAINT, &wxInputDraw::OnPaintEvent, this); }

void wxInputDraw::OnPaintEvent(wxPaintEvent& event)
{
	wxAutoBufferedPaintDC dc(this);
	OnRender(dc);
}

void wxInputDraw::OnRender(wxDC& dc)
{
	const bool isDark = CemuApp::IsDarkMode() || wxHelper::IsDarkColour(GetBackgroundColour());
	dc.SetBackgroundMode(wxSOLID);
	dc.SetBackground(wxBrush(isDark ? wxColour(35, 35, 38) : *wxWHITE));
	dc.Clear();

	glm::vec2 position;
	wxPen fgPen(isDark ? wxColour(220, 220, 225) : *wxBLACK);
	wxBrush fgBrush(isDark ? wxColour(220, 220, 225) : *wxBLACK);
	wxPen deadzonePen(isDark ? wxColour(110, 110, 120) : wxColour(128, 128, 128));
	wxBrush deadzoneBrush(isDark ? wxColour(58, 58, 66) : *wxLIGHT_GREY);
	const wxPen* red = wxRED_PEN;
	const wxBrush* red_brush = wxRED_BRUSH;
	wxColour activeGreen = isDark ? wxColour(76, 224, 122) : wxColour(0x336600);

	if (IsEnabled())
	{
		position = m_position;
	}
	else
	{
		position = {};
		fgPen = wxPen(wxColour(128, 128, 128));
		fgBrush = wxBrush(wxColour(128, 128, 128));
		red = wxMEDIUM_GREY_PEN;
		red_brush = wxMEDIUM_GREY_BRUSH;
	}

	const auto size = GetSize();
	const auto min_size = (float)std::min(size.GetWidth(), size.GetHeight()) - 1.0f;
	const Vector2f middle{min_size / 2.0f, min_size / 2.0f};

	// border
	const wxRect border{0, 0, (int)min_size, (int)min_size};
	dc.SetPen(fgPen);
	dc.DrawRectangle(border);

	dc.SetPen(IsEnabled() ? wxPen(activeGreen) : deadzonePen);
	dc.DrawCircle((int)middle.x, (int)middle.y, (int)middle.x);

	if (m_deadzone > 0)
	{
		dc.SetPen(deadzonePen);
		dc.SetBrush(deadzoneBrush);
		const auto deadzone_size = m_deadzone * min_size / 2.0f;
		dc.DrawCircle(
			static_cast<int>(middle.x),
			static_cast<int>(middle.x),
			(int)deadzone_size);

		if (length(position) >= m_deadzone)
		{
			dc.SetPen(*red);
			dc.SetBrush(*red_brush);

			if (std::abs(1.0f - length(position)) < 0.05f)
			{
				dc.SetPen(wxPen(activeGreen));
				dc.SetBrush(wxBrush(activeGreen));
			}
		}
		else
		{
			dc.SetPen(fgPen);
			dc.SetBrush(fgBrush);
		}
	}
	else
	{
		dc.SetPen(*red);
		dc.SetBrush(*red_brush);
	}

	// draw axis
	const wxPoint pos
	{
		static_cast<int>(middle.x + position.x * middle.x),
		static_cast<int>(middle.y - position.y * middle.y)
	};
	dc.DrawCircle(pos.x, pos.y, 2);

	dc.SetBrush(*wxTRANSPARENT_BRUSH);
}

void wxInputDraw::SetAxisValue(const glm::vec2& position)
{
	m_position = position;
	Refresh();
}

void wxInputDraw::SetDeadzone(float deadzone)
{
	m_deadzone = deadzone;
	Refresh();
}
