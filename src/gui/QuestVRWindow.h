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

	// Quest VR / Bluetooth Gamepad input bridge for emulated controllers
	static void SetQuestInput(int player, uint32_t buttons, float lx, float ly, float rx, float ry);
	static bool GetQuestInput(int player, uint32_t& buttons, float& lx, float& ly, float& rx, float& ry);
	static bool HasRecentQuestInput(int player);
	static uint32_t ConvertVpadToWpadButtons(uint32_t vpad);

	// ADB WebXR 1-Click Fix & Quest flags automation
	static std::string FindAdbExecutable();
	static bool RunAdbCommand(const std::string& args, std::string* output = nullptr);

private:
	void OnStartStop(wxCommandEvent& event);
	void OnCopyUrl(wxCommandEvent& event);
	void OnPreviewBrowser(wxCommandEvent& event);
	void OnAdbFixWebXR(wxCommandEvent& event);
	void OnOpenQuestFlagsGuide(wxCommandEvent& event);
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
	wxButton* m_adbFixBtn{};
	wxButton* m_flagsGuideBtn{};
	wxTimer* m_uiTimer{};
};
