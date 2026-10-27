#pragma once

#include <wx/dialog.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <cstdint>
#include <string>

class QuestVRWindow : public wxDialog
{
public:
	explicit QuestVRWindow(wxWindow* parent);
	~QuestVRWindow() override;

	static void SetMainAndPadHWNDs(uintptr_t mainHwnd, uintptr_t padHwnd);
	static bool IsBroadcastRunning();
	static void ShutdownBroadcast();

private:
	void OnStartStop(wxCommandEvent& event);
	void OnCopyUrl(wxCommandEvent& event);
	void OnPreviewBrowser(wxCommandEvent& event);
	void OnOpenInputSettings(wxCommandEvent& event);
	void OnSettingsChanged(wxCommandEvent& event);
	void OnTimer(wxTimerEvent& event);
	void RefreshUI();

	wxChoice* m_playerChoice{};
	wxChoice* m_modeChoice{};
	wxChoice* m_fpsChoice{};
	wxChoice* m_qualityChoice{};
	wxTextCtrl* m_urlText{};
	wxStaticText* m_statusText{};
	wxStaticText* m_controllerStatusText{};
	wxButton* m_startStopBtn{};
	wxTimer* m_uiTimer{};
};
