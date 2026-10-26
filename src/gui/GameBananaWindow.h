#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <wx/dialog.h>

class wxButton;
class wxChoice;
class wxGauge;
class wxListCtrl;
class wxListEvent;
class wxNotebook;
class wxPanel;
class wxSearchCtrl;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

// Comprehensive GameBanana Modding Hub for Cemu:
// - Browse & download 3D models, textures, maps/stages, gameplay mods, audio, UI, shaders & cheats
// - Live preview thumbnails, multi-file selection, and automatic Cemu GraphicPack installation
// - Installed Mods Manager (enable/disable, import local .zip mods, open folder, delete)
// - Cheats & Code Patches Studio (built-in Wii U cheat templates + custom patches.txt generator)
class GameBananaWindow : public wxDialog
{
public:
	GameBananaWindow(wxWindow* parent, uint64 initialTitleId = 0);
	~GameBananaWindow() override;

private:
	struct FileEntry
	{
		uint64 id = 0;
		std::string fileName;
		std::string downloadUrl;
		std::string description;
		uint64 fileSize = 0;
		uint64 downloadCount = 0;
	};

	struct Entry
	{
		uint64 id = 0;
		std::string name;
		std::string gameName;
		uint64 gameId = 0;
		std::string category;
		std::string rootCategory;
		std::string author;
		std::string url;
		std::string thumbnailUrl;
		std::string version;
		std::string description;
		uint64 likeCount = 0;
		uint64 viewCount = 0;
		uint64 downloadCount = 0;
		std::vector<FileEntry> files;
		bool detailsLoaded = false;
	};

	struct InstalledModInfo
	{
		std::string name;
		std::string gameTitleIds;
		std::string contentType;
		fs::path folderPath;
		fs::path rulesPath;
		std::string normalizedPath;
		bool isGraphicPack = false;
		bool enabled = false;
	};

	// UI construction helpers
	wxPanel* CreateBrowseTab(wxWindow* parent);
	wxPanel* CreateInstalledModsTab(wxWindow* parent);
	wxPanel* CreateCheatsTab(wxWindow* parent);

	// Tab 1: Browse & Download handlers
	void OnSearch(wxCommandEvent& event);
	void OnGameChanged(wxCommandEvent& event);
	void OnCategoryChanged(wxCommandEvent& event);
	void OnSortChanged(wxCommandEvent& event);
	void OnPrevPage(wxCommandEvent& event);
	void OnNextPage(wxCommandEvent& event);
	void OnModSelected(wxListEvent& event);
	void OnOpenPage(wxCommandEvent& event);
	void OnDownloadAndInstall(wxCommandEvent& event);
	void OnOpenModsFolder(wxCommandEvent& event);

	void PerformSearch(int page = 1);
	void ApplyClientFiltersAndShow();
	void FetchModDetailsAndThumbnail(size_t visibleIndex);
	void UpdatePlaceholderBitmap();

	// Zip extraction & GraphicPack auto-packaging
	bool InstallZipAsGraphicPack(const std::vector<uint8>& zipBytes,
								 const std::string& modName,
								 const std::string& modAuthor,
								 const std::string& modCategory,
								 const std::string& modVersion,
								 const std::string& gameName,
								 const std::string& fallbackTitleIds,
								 fs::path& outInstalledDir,
								 std::string& outError);
	static void EnableAndReloadGraphicPack(const fs::path& installedDir);

	// Tab 2: Installed Mods Manager handlers
	void RefreshInstalledMods();
	void OnToggleInstalledMod(wxCommandEvent& event);
	void OnImportLocalZip(wxCommandEvent& event);
	void OnOpenSelectedModFolder(wxCommandEvent& event);
	void OnDeleteSelectedMod(wxCommandEvent& event);

	// Tab 3: Cheats & Code Patches Studio handlers
	void OnCheatGameChanged(wxCommandEvent& event);
	void OnCheatPresetChanged(wxCommandEvent& event);
	void OnSaveAndActivateCheat(wxCommandEvent& event);
	void OnSearchGameBananaCheats(wxCommandEvent& event);
	void PopulateCheatPresetsForGame(int gameIdx);

	void SetStatus(const wxString& text);
	bool MatchesSelectedCategory(const Entry& e) const;

	std::shared_ptr<std::atomic<bool>> m_alive;
	uint64 m_initialTitleId = 0;
	bool m_busy = false;
	int m_currentPage = 1;

	wxNotebook* m_notebook = nullptr;
	wxStaticText* m_status = nullptr;

	// Tab 1 widgets
	wxChoice* m_gameChoice = nullptr;
	wxChoice* m_categoryChoice = nullptr;
	wxChoice* m_sortChoice = nullptr;
	wxSearchCtrl* m_search = nullptr;
	wxButton* m_searchBtn = nullptr;
	wxButton* m_prevPageBtn = nullptr;
	wxStaticText* m_pageLabel = nullptr;
	wxButton* m_nextPageBtn = nullptr;
	wxListCtrl* m_list = nullptr;

	wxStaticBitmap* m_previewBitmap = nullptr;
	wxStaticText* m_detailTitle = nullptr;
	wxStaticText* m_detailMeta = nullptr;
	wxTextCtrl* m_detailDesc = nullptr;
	wxChoice* m_fileChoice = nullptr;
	wxGauge* m_downloadGauge = nullptr;
	wxButton* m_installModBtn = nullptr;
	wxButton* m_openWebBtn = nullptr;

	std::vector<Entry> m_rawEntries;
	std::vector<Entry> m_visibleEntries;
	long m_selectedVisibleIdx = -1;

	// Tab 2 widgets
	wxListCtrl* m_installedList = nullptr;
	std::vector<InstalledModInfo> m_installedMods;

	// Tab 3 widgets
	wxChoice* m_cheatGameChoice = nullptr;
	wxChoice* m_cheatPresetChoice = nullptr;
	wxTextCtrl* m_cheatNameCtrl = nullptr;
	wxTextCtrl* m_cheatTitleIdsCtrl = nullptr;
	wxTextCtrl* m_cheatCodeCtrl = nullptr;
};
