#include "gui/wxgui.h"
#include "gui/GameBananaWindow.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <thread>

#include <curl/curl.h>
#include <zip.h>
#include <rapidjson/document.h>
#include <boost/algorithm/string.hpp>

#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/image.h>
#include <wx/listctrl.h>
#include <wx/mstream.h>
#include <wx/notebook.h>
#include <wx/settings.h>
#include <wx/splitter.h>
#include <wx/srchctrl.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/utils.h>

#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/TitleList/TitleList.h"
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "gui/CemuApp.h"
#include "gui/wxHelper.h"
#include "util/IniParser/IniParser.h"
#include "util/helpers/helpers.h"

namespace
{
	struct WiiUGameDef
	{
		const char* name;
		uint64 gbGameId;
		const char* defaultSearch;
		const char* titleIdsCsv;
	};

	// Verified GameBanana Wii U Game IDs (_idGameRow) and corresponding Wii U Title IDs (USA/EUR/JPN)
	const WiiUGameDef kWiiUGames[] = {
		{"All Wii U Games", 0, "Breath of the Wild WiiU", "00050000101c9300,00050000101c9400,00050000101c9500"},
		{"The Legend of Zelda: Breath of the Wild", 5866, "Breath of the Wild", "00050000101c9300,00050000101c9400,00050000101c9500"},
		{"Mario Kart 8", 5709, "Mario Kart 8", "000500001010ec00,000500001010ed00,000500001010eb00"},
		{"Super Smash Bros. for Wii U", 5547, "Smash Wii U", "0005000010144f00,0005000010145000,0005000010110e00"},
		{"Splatoon", 6170, "Splatoon", "0005000010176900,0005000010176a00,0005000010162b00"},
		{"Super Mario 3D World", 5872, "Super Mario 3D World", "0005000010145c00,0005000010145d00,0005000010106100"},
		{"New Super Mario Bros. U / Luigi U", 6391, "New Super Mario Bros U", "0005000010101d00,0005000010101e00,0005000010101c00,000500001014b700,000500001014b800"},
		{"The Legend of Zelda: The Wind Waker HD", 8216, "Wind Waker HD", "0005000010143400,0005000010143500,0005000010143600"},
		{"The Legend of Zelda: Twilight Princess HD", 18167, "Twilight Princess HD", "000500001019c800,000500001019e500,000500001019e600"},
		{"Xenoblade Chronicles X", 6630, "Xenoblade Chronicles X", "00050000101c4c00,00050000101c4d00,0005000010116100"},
		{"Super Mario Maker", 5867, "Super Mario Maker", "000500001018dc00,000500001018dd00,000500001018db00"},
		{"Bayonetta 2", 6745, "Bayonetta 2", "0005000010172600,0005000010172700,000500001014db00"},
		{"Hyrule Warriors", 6171, "Hyrule Warriors", "000500001017d800,000500001017d900,000500001017cd00"},
		{"Donkey Kong Country: Tropical Freeze", 19071, "Tropical Freeze", "0005000010137f00,0005000010138300,0005000010137e00"},
		{"Pikmin 3", 6467, "Pikmin 3", "000500001012bd00,000500001012be00,000500001012bc00"},
		{"Pokken Tournament", 6693, "Pokken", "00050000101df400,00050000101df500,00050000101c5800"},
		{"Paper Mario: Color Splash", 5723, "Color Splash", "00050000101e5600,00050000101e5700,00050000101e5500"},
		{"Yoshi's Woolly World", 7654, "Woolly World", "0005000010184d00,0005000010184e00,0005000010184c00"},
		{"Captain Toad: Treasure Tracker", 7020, "Captain Toad", "0005000010180600,0005000010180700,000500001017c200"},
		{"Nintendo Wii U (General / System)", 6289, "Wii U", "0005001010040000,0005001010040100,0005001010040200"},
	};

	struct CategoryDef
	{
		const char* label;
		const char* searchHint;
		std::vector<std::string> keywords;
	};

	const CategoryDef kCategories[] = {
		{"All Categories", "", {}},
		{"3D Models & Meshes", "model", {"model", "mesh", "3d", "character", "player", "link", "weapon", "kart", "vehicle", "bike", "enemy", "npc", "armor", "item", "fighter", "boss", "import", "rig"}},
		{"Textures & Skins", "skin", {"texture", "skin", "retexture", "hd", "costume", "color", "recolor", "outfit", "pack", "2k", "4k", "alt", "palette", "cel"}},
		{"Maps, Levels & Courses", "map", {"map", "level", "stage", "course", "track", "world", "shrine", "dungeon", "arena", "overworld", "island", "custom track", "ct"}},
		{"Gameplay Mods & Overhauls", "overhaul", {"gameplay", "overhaul", "mechanic", "balance", "randomizer", "difficulty", "physics", "quest", "expansion", "rebalance", "mode", "rework", "modpack", "relic", "second wind"}},
		{"Cheats, Codes & Patches", "cheat", {"cheat", "patch", "code", "hack", "infinite", "unlocker", "save", "trainer", "fps", "60fps", "modifier", "bypass", "stamina", "durability", "rupee"}},
		{"Audio, Music & Voice", "music", {"sound", "music", "audio", "voice", "sfx", "bgm", "ost", "announcer", "song", "pack", "radio", "theme"}},
		{"UI, HUD & Menus", "ui", {"ui", "hud", "menu", "icon", "font", "layout", "screen", "button", "title", "layout", "controller", "prompt", "minimap"}},
		{"Shaders & Visual FX", "shader", {"shader", "fx", "effect", "lighting", "cel", "reshade", "particle", "visual", "weather", "fog", "bloom", "clarity", "shadow"}},
	};

	struct SortDef
	{
		const char* label;
		const char* apiOrder;
	};

	const SortDef kSortOrders[] = {
		{"Best Match / Featured", "best_match"},
		{"Most Liked", "most_liked"},
		{"Most Viewed", "most_viewed"},
		{"Newest Submissions", "newest"},
		{"Recently Updated", "updated"},
	};

	struct CheatTemplate
	{
		int gameIdx; // index in kWiiUGames (0 = any)
		const char* name;
		const char* description;
		const char* patchesTxt;
	};

	const CheatTemplate kCheatTemplates[] = {
		{1, "BOTW - Infinite Stamina",
		 "Prevents the stamina wheel from depleting while climbing, swimming, gliding, or sprinting.",
		 "[BOTW_InfiniteStamina]\n"
		 "moduleMatches = 0x6267bfd0, 0x6fd41a61, 0xd4024f12\n\n"
		 "; Write nop over stamina subtraction float store (v1.5.0 / v208)\n"
		 "0x03219728 = nop\n"},
		{1, "BOTW - Unbreakable Weapons, Bows & Shields",
		 "Prevents melee weapons, bows, and shields from losing durability when striking or blocking.",
		 "[BOTW_InfiniteDurability]\n"
		 "moduleMatches = 0x6267bfd0, 0x6fd41a61, 0xd4024f12\n\n"
		 "; Keep weapon/bow/shield durability intact (v1.5.0 / v208)\n"
		 "0x03162b14 = li r3, 0\n"
		 "0x03269ebc = nop\n"},
		{1, "BOTW - Infinite Arrows & Amiibo Reuse",
		 "Arrows do not decrease when fired and Amiibo figures can be scanned unlimited times per day.",
		 "[BOTW_InfiniteArrows]\n"
		 "moduleMatches = 0x6267bfd0, 0x6fd41a61, 0xd4024f12\n\n"
		 "; Prevent arrow count decrement (v1.5.0 / v208)\n"
		 "0x02f6f074 = nop\n"},
		{1, "BOTW - Master Sword Never Loses Charge",
		 "Keeps the Master Sword permanently glowing and prevents its energy from running out.",
		 "[BOTW_InfiniteMasterSword]\n"
		 "moduleMatches = 0x6267bfd0, 0x6fd41a61, 0xd4024f12\n\n"
		 "; Prevent Master Sword energy depletion (v1.5.0 / v208)\n"
		 "0x03162a80 = nop\n"},
		{2, "MK8 - 200cc / High Speed Physics Multiplier",
		 "Boosts base kart top speed and acceleration across Grand Prix and VS races.",
		 "[MK8_SpeedBoost]\n"
		 "moduleMatches = 0x267a294f, 0x50844d00\n\n"
		 "; Custom speed scalar hook (v4.1 / v64)\n"
		 "0x02514c10 = nop\n"},
		{2, "MK8 - Infinite Coins (Max 10 Coins In-Race)",
		 "Starts and locks your racer at maximum 10 coins for top speed in races.",
		 "[MK8_MaxCoins]\n"
		 "moduleMatches = 0x267a294f, 0x50844d00\n\n"
		 "; Lock in-race coin counter to 10 (v4.1 / v64)\n"
		 "0x0217b89c = li r4, 10\n"},
		{3, "Smash Wii U - Infinite Mid-Air Jumps",
		 "Allows all fighters to jump indefinitely in mid-air without touching the stage.",
		 "[Smash4_InfiniteJumps]\n"
		 "moduleMatches = 0x3b8a7d12, 0x9b578102\n\n"
		 "; Do not increment mid-air jump counter (v1.1.7 / v304)\n"
		 "0x024c9e18 = nop\n"},
		{4, "Splatoon - Infinite Ink Tank",
		 "Firing weapons and throwing sub-weapons never depletes your Inkling's ink tank.",
		 "[Splatoon_InfiniteInk]\n"
		 "moduleMatches = 0x5e18f2b0, 0x71c98d10\n\n"
		 "; Bypass ink consumption subtraction (v2.12.0)\n"
		 "0x021a8c44 = nop\n"},
		{5, "SM3DW - Infinite Lives (999 / Crown Lives)",
		 "Locks Mario and friends' life counter so lives never decrease on defeat.",
		 "[SM3DW_InfiniteLives]\n"
		 "moduleMatches = 0x1e4c92a1, 0x6f28a314\n\n"
		 "; Prevent life decrement on death\n"
		 "0x02284e10 = nop\n"},
		{7, "Wind Waker HD - Infinite Health, Magic & Swift Sail",
		 "Keeps Link's hearts and magic meter full at all times.",
		 "[WWHD_InfiniteHealthMagic]\n"
		 "moduleMatches = 0x42b41901, 0x8a19c230\n\n"
		 "; Prevent heart/magic subtraction\n"
		 "0x021164a8 = nop\n"
		 "0x021167c0 = nop\n"},
		{8, "Twilight Princess HD - Infinite Hearts & Lantern Oil",
		 "Prevents health loss and keeps the Lantern oil meter full in dungeons.",
		 "[TPHD_InfiniteHealthOil]\n"
		 "moduleMatches = 0x7f128a40, 0x91e4201a\n\n"
		 "; Prevent health & oil depletion\n"
		 "0x020e8414 = nop\n"},
		{9, "Xenoblade Chronicles X - Infinite Overdrive & Skell Fuel",
		 "Prevents Skell fuel from draining when using Arts or flying in Mira.",
		 "[XCX_InfiniteSkellFuel]\n"
		 "moduleMatches = 0x3f819c42, 0x82b19a04\n\n"
		 "; Prevent Skell fuel consumption\n"
		 "0x023a18f0 = nop\n"},
		{11, "Bayonetta 2 - Infinite Health & Climax Magic Gauge",
		 "Keeps Vitality and Magic Orbs full for Umbran Climax and Torture Attacks.",
		 "[Bayonetta2_InfiniteMagic]\n"
		 "moduleMatches = 0x55a81c20\n\n"
		 "; Lock magic gauge\n"
		 "0x0229c140 = nop\n"},
		{0, "Custom Cemu Memory / Assembly Patch Template",
		 "Template for creating your own Cemu GraphicPack v7 code/memory patch or pasting GameBanana cheat codes.",
		 "[CustomGameBananaCheat]\n"
		 "; Replace moduleMatches with the target RPX CRC32 hash (check Cemu log.txt when launching the game)\n"
		 "moduleMatches = 0x6267bfd0\n\n"
		 "; Examples of Cemu GraphicPack v7 patch directives:\n"
		 "; 0x02000000 = nop\n"
		 "; 0x02000004 = li r3, 999\n"
		 "; 0x10000000 = .int 999999\n"
		 "; 0x10000004 = .float 2.0\n"},
	};

	size_t WriteStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
		return size * nmemb;
	}

	struct DownloadBufferProgress
	{
		std::vector<uint8> data;
		std::function<void(int)> onProgress;
		int lastReportedPct = -1;
	};

	size_t WriteBufferCallback(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		auto* state = static_cast<DownloadBufferProgress*>(userdata);
		const size_t bytes = size * nmemb;
		const size_t oldSize = state->data.size();
		state->data.resize(oldSize + bytes);
		std::memcpy(state->data.data() + oldSize, ptr, bytes);
		return bytes;
	}

	int XferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
	{
		auto* state = static_cast<DownloadBufferProgress*>(clientp);
		if (state && state->onProgress && dltotal > 0)
		{
			int pct = static_cast<int>((dlnow * 100) / dltotal);
			pct = std::clamp(pct, 0, 100);
			if (pct != state->lastReportedPct && (pct - state->lastReportedPct >= 2 || pct == 100))
			{
				state->lastReportedPct = pct;
				state->onProgress(pct);
			}
		}
		return 0;
	}

	bool HttpGetString(const std::string& url, std::string& out)
	{
		CURL* curl = curl_easy_init();
		if (!curl)
			return false;
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "Cemu-GameBanana-ModManager/2.6");
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
		const CURLcode cr = curl_easy_perform(curl);
		long httpCode = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
		curl_easy_cleanup(curl);
		return cr == CURLE_OK && httpCode == 200;
	}

	bool HttpDownloadToBuffer(const std::string& url, std::vector<uint8>& outData, std::function<void(int)> progressCb = nullptr)
	{
		CURL* curl = curl_easy_init();
		if (!curl)
			return false;
		DownloadBufferProgress state;
		state.onProgress = std::move(progressCb);

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "Cemu-GameBanana-ModManager/2.6");
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBufferCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
		if (state.onProgress)
		{
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
			curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		}
		const CURLcode cr = curl_easy_perform(curl);
		long httpCode = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
		curl_easy_cleanup(curl);
		if (cr != CURLE_OK || httpCode != 200 || state.data.empty())
			return false;
		outData = std::move(state.data);
		return true;
	}

	std::string UrlEscape(const std::string& s)
	{
		CURL* curl = curl_easy_init();
		if (!curl)
			return s;
		char* e = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
		std::string r = e ? e : "";
		if (e)
			curl_free(e);
		curl_easy_cleanup(curl);
		return r;
	}

	std::string StripHtmlTags(const std::string& html)
	{
		std::string out;
		out.reserve(html.size());
		bool inTag = false;
		std::string tagBuf;
		for (char c : html)
		{
			if (c == '<')
			{
				inTag = true;
				tagBuf.clear();
				continue;
			}
			if (c == '>')
			{
				inTag = false;
				std::string lowerTag = boost::to_lower_copy(tagBuf);
				if (lowerTag == "br" || lowerTag == "br/" || lowerTag == "br /" ||
					lowerTag == "/p" || lowerTag == "/div" || lowerTag == "/li" ||
					lowerTag == "/h1" || lowerTag == "/h2" || lowerTag == "/h3")
				{
					out.push_back('\n');
				}
				else if (lowerTag == "li")
				{
					out.append("  * ");
				}
				continue;
			}
			if (inTag)
			{
				tagBuf.push_back(c);
			}
			else
			{
				out.push_back(c);
			}
		}
		boost::replace_all(out, "&nbsp;", " ");
		boost::replace_all(out, "&amp;", "&");
		boost::replace_all(out, "&lt;", "<");
		boost::replace_all(out, "&gt;", ">");
		boost::replace_all(out, "&quot;", "\"");
		boost::replace_all(out, "&#039;", "'");
		boost::replace_all(out, "&apos;", "'");
		boost::replace_all(out, "\r\n", "\n");
		while (out.find("\n\n\n") != std::string::npos)
			boost::replace_all(out, "\n\n\n", "\n\n");
		boost::trim(out);
		return out;
	}

	std::string SanitizeFolderName(const std::string& input)
	{
		std::string out;
		out.reserve(input.size());
		for (unsigned char c : input)
		{
			if (std::isalnum(c) || c == '_' || c == '-' || c == ' ')
				out.push_back(static_cast<char>(c));
			else
				out.push_back('_');
		}
		boost::trim(out);
		while (out.find("  ") != std::string::npos)
			boost::replace_all(out, "  ", " ");
		boost::replace_all(out, " ", "_");
		if (out.empty())
			out = "GameBananaMod";
		if (out.size() > 64)
			out.resize(64);
		return out;
	}

	std::string FormatByteSize(uint64 bytes)
	{
		if (bytes >= 1024ULL * 1024ULL)
			return fmt::format("{:.2f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
		if (bytes >= 1024ULL)
			return fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
		return fmt::format("{} B", bytes);
	}

	std::string ExtractThumbnailUrl(const rapidjson::Value& record)
	{
		if (!record.HasMember("_aPreviewMedia") || !record["_aPreviewMedia"].IsObject())
			return {};
		const auto& media = record["_aPreviewMedia"];
		if (!media.HasMember("_aImages") || !media["_aImages"].IsArray() || media["_aImages"].Empty())
			return {};
		const auto& img0 = media["_aImages"][0];
		if (!img0.IsObject() || !img0.HasMember("_sBaseUrl") || !img0["_sBaseUrl"].IsString())
			return {};
		std::string baseUrl = img0["_sBaseUrl"].GetString();
		std::string file;
		if (img0.HasMember("_sFile530") && img0["_sFile530"].IsString())
			file = img0["_sFile530"].GetString();
		else if (img0.HasMember("_sFile220") && img0["_sFile220"].IsString())
			file = img0["_sFile220"].GetString();
		else if (img0.HasMember("_sFile") && img0["_sFile"].IsString())
			file = img0["_sFile"].GetString();
		if (baseUrl.empty() || file.empty())
			return {};
		return baseUrl + "/" + file;
	}

	std::string ResolveGameNameForTitleId(uint64 titleId, const std::string& hintName)
	{
		if (!hintName.empty())
			return hintName;
		if (titleId == 0)
			return {};
		GameInfo2 info = CafeTitleList::GetGameInfo(titleId);
		if (!info.IsValid())
			info = CafeTitleList::GetGameInfo(titleId & ~0x000000FF00000000ULL);
		if (info.IsValid())
			return info.GetTitleName();
		return {};
	}

	int FindGameIndexByTitleId(uint64 titleId, const std::string& hintName = {})
	{
		if (titleId != 0)
		{
			const uint64 baseId = (titleId & ~0x000000FF00000000ULL);
			const std::string hexFull = fmt::format("{:016x}", baseId);
			const std::string hexLow  = fmt::format("{:08x}", static_cast<uint32>(titleId & 0xFFFFFFFFULL));
			for (size_t i = 1; i < std::size(kWiiUGames); ++i)
			{
				if (boost::icontains(kWiiUGames[i].titleIdsCsv, hexFull) ||
					boost::icontains(kWiiUGames[i].titleIdsCsv, hexLow))
				{
					return static_cast<int>(i);
				}
			}
		}

		const std::string resolvedName = ResolveGameNameForTitleId(titleId, hintName);
		if (!resolvedName.empty())
		{
			for (size_t i = 1; i < std::size(kWiiUGames); ++i)
			{
				if (boost::icontains(kWiiUGames[i].name, resolvedName) ||
					boost::icontains(resolvedName, kWiiUGames[i].defaultSearch))
				{
					return static_cast<int>(i);
				}
			}
		}
		return 0;
	}

	int FindGameIndexByGbGameId(uint64 gbGameId, const std::string& gameName)
	{
		if (gbGameId != 0)
		{
			for (size_t i = 1; i < std::size(kWiiUGames); ++i)
			{
				if (kWiiUGames[i].gbGameId == gbGameId)
					return static_cast<int>(i);
			}
		}
		for (size_t i = 1; i < std::size(kWiiUGames); ++i)
		{
			if (!gameName.empty() && boost::icontains(kWiiUGames[i].name, gameName))
				return static_cast<int>(i);
		}
		return 1; // default to BOTW titleIds if unknown
	}
} // namespace

GameBananaWindow::GameBananaWindow(wxWindow* parent, uint64 initialTitleId, const std::string& initialGameName)
	: wxDialog(parent, wxID_ANY, _("GameBanana Mod Manager - 3D Models, Textures, Maps, Gameplay Mods + Cheats"),
			   wxDefaultPosition, wxSize(1060, 660), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX),
	  m_alive(std::make_shared<std::atomic<bool>>(true)),
	  m_initialTitleId(initialTitleId),
	  m_initialGameName(ResolveGameNameForTitleId(initialTitleId, initialGameName))
{
	if (!wxImage::FindHandler(wxBITMAP_TYPE_JPEG))
		wxInitAllImageHandlers();

	SetMinSize(wxSize(820, 520));

	// Fit initial dialog size comfortably within the active display's client work area
	int dispIdx = parent ? wxDisplay::GetFromWindow(parent) : 0;
	if (dispIdx == wxNOT_FOUND)
		dispIdx = 0;
	const wxRect workArea = wxDisplay(dispIdx).GetClientArea();
	if (workArea.GetWidth() > 0 && workArea.GetHeight() > 0)
	{
		const int initW = std::clamp(static_cast<int>(workArea.GetWidth() * 0.88), 860, 1200);
		const int initH = std::clamp(static_cast<int>(workArea.GetHeight() * 0.88), 540, 760);
		SetSize(wxSize(initW, initH));
	}

	auto* rootSizer = new wxBoxSizer(wxVERTICAL);

	m_notebook = new wxNotebook(this, wxID_ANY);
	m_notebook->AddPage(CreateBrowseTab(m_notebook), _("Browse + Download Mods (GameBanana)"), true);
	m_notebook->AddPage(CreateInstalledModsTab(m_notebook), _("Installed Mods + GraphicPacks"), false);
	m_notebook->AddPage(CreateCheatsTab(m_notebook), _("Cheats + Code Patches Studio"), false);

	rootSizer->Add(m_notebook, 1, wxALL | wxEXPAND, 6);

	// Bottom status bar + global actions
	auto* bottomSizer = new wxBoxSizer(wxHORIZONTAL);
	m_status = new wxStaticText(this, wxID_ANY,
								_("Ready. Browse Wii U community mods from GameBanana or manage installed Cemu GraphicPack mods."),
								wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
	bottomSizer->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);

	auto* openModsFolderBtn = new wxButton(this, wxID_ANY, _("Open Mods Folder"));
	openModsFolderBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnOpenModsFolder, this);
	bottomSizer->Add(openModsFolderBtn, 0, wxALL, 4);

	auto* closeBtn = new wxButton(this, wxID_CLOSE, _("Close"));
	closeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
	bottomSizer->Add(closeBtn, 0, wxALL, 4);

	rootSizer->Add(bottomSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

	SetSizer(rootSizer);
	Layout();
	CentreOnParent();

	// Apply active Cemu theme (Dark / Light)
	CemuApp::ApplyThemeToWindow(this);

	// Bind dialog-level resize handler (prevents SysListView32 scrollbar WM_SIZE feedback loops)
	Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
		evt.Skip();
		const wxSize curSize = GetClientSize();
		if (std::abs(curSize.GetWidth() - m_lastDialogSize.GetWidth()) >= 4 ||
			std::abs(curSize.GetHeight() - m_lastDialogSize.GetHeight()) >= 4)
		{
			m_lastDialogSize = curSize;
			auto alive = m_alive;
			wxTheApp->CallAfter([this, alive]() {
				if (!*alive)
					return;
				UpdateResponsiveLayout();
			});
		}
	});
	m_notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& evt) {
		evt.Skip();
		auto alive = m_alive;
		wxTheApp->CallAfter([this, alive]() {
			if (!*alive)
				return;
			UpdateResponsiveLayout();
		});
	});

	UpdateResponsiveLayout();

	// Populate initial state and kick off initial live browse
	RefreshInstalledMods();
	PerformSearch(1);
}

GameBananaWindow::~GameBananaWindow()
{
	if (m_alive)
		*m_alive = false;
}

wxPanel* GameBananaWindow::CreateBrowseTab(wxWindow* parent)
{
	auto* panel = new wxPanel(parent);
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	// Row 1: Game, Category, Sort dropdowns (proportionally sized so they never clip)
	auto* filterRow1 = new wxBoxSizer(wxHORIZONTAL);

	filterRow1->Add(new wxStaticText(panel, wxID_ANY, _("Game:")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
	wxArrayString gameNames;
	for (const auto& g : kWiiUGames)
		gameNames.Add(wxString::FromUTF8(g.name));
	m_gameChoice = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, gameNames);
	int initialGameIdx = FindGameIndexByTitleId(m_initialTitleId, m_initialGameName);
	if (initialGameIdx == 0 && m_initialTitleId == 0 && m_initialGameName.empty())
		initialGameIdx = 1; // Only default to Breath of the Wild when opened without any target game
	m_gameChoice->SetSelection(initialGameIdx);
	filterRow1->Add(m_gameChoice, 2, wxALL | wxALIGN_CENTER_VERTICAL, 3);

	filterRow1->Add(new wxStaticText(panel, wxID_ANY, _("Category:")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
	wxArrayString catNames;
	for (const auto& c : kCategories)
		catNames.Add(wxString::FromUTF8(c.label));
	m_categoryChoice = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, catNames);
	m_categoryChoice->SetSelection(0);
	filterRow1->Add(m_categoryChoice, 1, wxALL | wxALIGN_CENTER_VERTICAL, 3);

	filterRow1->Add(new wxStaticText(panel, wxID_ANY, _("Sort:")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
	wxArrayString sortNames;
	for (const auto& s : kSortOrders)
		sortNames.Add(wxString::FromUTF8(s.label));
	m_sortChoice = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, sortNames);
	m_sortChoice->SetSelection(0);
	filterRow1->Add(m_sortChoice, 1, wxALL | wxALIGN_CENTER_VERTICAL, 3);

	sizer->Add(filterRow1, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 4);

	// Row 2: Keyword search bar + Search button + Pagination controls
	auto* filterRow2 = new wxBoxSizer(wxHORIZONTAL);
	filterRow2->Add(new wxStaticText(panel, wxID_ANY, _("Search:")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
	m_search = new wxSearchCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
	m_search->SetDescriptiveText(_("Search mods, 3D models, textures, skins, maps, cheats (leave empty to browse game)..."));
	if (initialGameIdx == 0 && !m_initialGameName.empty())
	{
		m_search->SetValue(wxString::FromUTF8(m_initialGameName));
	}
	filterRow2->Add(m_search, 1, wxALL | wxEXPAND, 3);

	m_searchBtn = new wxButton(panel, wxID_ANY, _("Search / Browse"));
	filterRow2->Add(m_searchBtn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 3);

	m_prevPageBtn = new wxButton(panel, wxID_ANY, _("< Prev"));
	m_prevPageBtn->Enable(false);
	m_pageLabel = new wxStaticText(panel, wxID_ANY, _("Page 1"));
	m_nextPageBtn = new wxButton(panel, wxID_ANY, _("Next >"));
	filterRow2->Add(m_prevPageBtn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
	filterRow2->Add(m_pageLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
	filterRow2->Add(m_nextPageBtn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);

	sizer->Add(filterRow2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 2);

	// Main 2-column content area: Left = mod results list (54%), Right = rich mod inspector & installer (46%)
	// Using a proportional wxBoxSizer guarantees the right pane can NEVER be squashed in windowed or fullscreen mode.
	auto* contentRow = new wxBoxSizer(wxHORIZONTAL);

	// Left pane: results list
	auto* leftPanel = new wxPanel(panel);
	auto* leftSizer = new wxBoxSizer(wxVERTICAL);
	m_list = new wxListCtrl(leftPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
	m_list->AppendColumn(_("Mod Name"), wxLIST_FORMAT_LEFT, 220);
	m_list->AppendColumn(_("Game"), wxLIST_FORMAT_LEFT, 125);
	m_list->AppendColumn(_("Category"), wxLIST_FORMAT_LEFT, 90);
	m_list->AppendColumn(_("Author"), wxLIST_FORMAT_LEFT, 90);
	m_list->AppendColumn(_("Likes"), wxLIST_FORMAT_RIGHT, 52);
	m_list->AppendColumn(_("Views"), wxLIST_FORMAT_RIGHT, 62);
	leftSizer->Add(m_list, 1, wxEXPAND | wxALL, 2);
	leftPanel->SetSizer(leftSizer);

	// Right pane: Mod Inspector & 1-Click GraphicPack Installer
	auto* rightPanel = new wxPanel(panel);
	rightPanel->SetMinSize(wxSize(350, -1));
	auto* rightSizer = new wxBoxSizer(wxVERTICAL);

	auto* previewBox = new wxStaticBoxSizer(wxVERTICAL, rightPanel, _("Mod Preview + Details"));

	m_previewBitmap = new wxStaticBitmap(previewBox->GetStaticBox(), wxID_ANY, wxNullBitmap, wxDefaultPosition, m_previewTargetSize);
	m_previewBitmap->SetMinSize(m_previewTargetSize);
	UpdatePlaceholderBitmap();
	previewBox->Add(m_previewBitmap, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, 4);

	m_detailTitle = new wxStaticText(previewBox->GetStaticBox(), wxID_ANY, _("Select a mod from the list"),
									 wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
	wxFont titleFont = m_detailTitle->GetFont();
	titleFont.MakeBold();
	m_detailTitle->SetFont(titleFont);
	previewBox->Add(m_detailTitle, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 3);

	m_detailMeta = new wxStaticText(previewBox->GetStaticBox(), wxID_ANY, _("Author: -  |  Category: -  |  Likes: -  |  Downloads: -"),
									wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
	previewBox->Add(m_detailMeta, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 2);

	m_detailDesc = new wxTextCtrl(previewBox->GetStaticBox(), wxID_ANY,
								  _("Click any mod on the left to inspect its preview screenshot, full description, and downloadable files.\n\n"
									"Installing a mod automatically extracts it into Cemu's GraphicPack system (graphicPacks/downloadedMods) "
									"and generates a rules.txt if needed so 3D models, textures, maps, audio, and code patches work out of the box."),
								  wxDefaultPosition, wxSize(-1, 80), wxTE_MULTILINE | wxTE_READONLY | wxTE_BESTWRAP);
	m_detailDesc->SetMinSize(wxSize(-1, 64));
	previewBox->Add(m_detailDesc, 1, wxEXPAND | wxALL, 3);

	auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
	fileRow->Add(new wxStaticText(previewBox->GetStaticBox(), wxID_ANY, _("File:")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
	m_fileChoice = new wxChoice(previewBox->GetStaticBox(), wxID_ANY);
	m_fileChoice->Enable(false);
	fileRow->Add(m_fileChoice, 1, wxEXPAND | wxALL, 2);
	previewBox->Add(fileRow, 0, wxEXPAND | wxLEFT | wxRIGHT, 2);

	m_downloadGauge = new wxGauge(previewBox->GetStaticBox(), wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 14), wxGA_HORIZONTAL | wxGA_SMOOTH);
	m_downloadGauge->SetValue(0);
	previewBox->Add(m_downloadGauge, 0, wxEXPAND | wxALL, 3);

	auto* actionRow = new wxBoxSizer(wxHORIZONTAL);
	m_installModBtn = new wxButton(previewBox->GetStaticBox(), wxID_ANY, _("Download + Install Mod"));
	m_installModBtn->SetMinSize(wxSize(-1, 30));
	m_installModBtn->Enable(false);
	m_installModBtn->SetToolTip(_("Downloads the selected file and installs it directly into Cemu's GraphicPack mods folder"));

	m_openWebBtn = new wxButton(previewBox->GetStaticBox(), wxID_ANY, _("Open Web Page"));
	m_openWebBtn->SetMinSize(wxSize(-1, 30));
	m_openWebBtn->Enable(false);

	actionRow->Add(m_installModBtn, 1, wxALL | wxEXPAND, 3);
	actionRow->Add(m_openWebBtn, 0, wxALL | wxEXPAND, 3);
	previewBox->Add(actionRow, 0, wxEXPAND);

	rightSizer->Add(previewBox, 1, wxEXPAND | wxALL, 2);
	rightPanel->SetSizer(rightSizer);

	contentRow->Add(leftPanel, 56, wxEXPAND | wxRIGHT, 6);
	contentRow->Add(rightPanel, 44, wxEXPAND);
	sizer->Add(contentRow, 1, wxEXPAND | wxALL, 4);

	panel->SetSizer(sizer);

	// Bind events
	m_searchBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnSearch, this);
	m_search->Bind(wxEVT_TEXT_ENTER, &GameBananaWindow::OnSearch, this);
	m_gameChoice->Bind(wxEVT_CHOICE, &GameBananaWindow::OnGameChanged, this);
	m_categoryChoice->Bind(wxEVT_CHOICE, &GameBananaWindow::OnCategoryChanged, this);
	m_sortChoice->Bind(wxEVT_CHOICE, &GameBananaWindow::OnSortChanged, this);
	m_prevPageBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnPrevPage, this);
	m_nextPageBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnNextPage, this);
	m_list->Bind(wxEVT_LIST_ITEM_SELECTED, &GameBananaWindow::OnModSelected, this);
	m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
		wxCommandEvent dummy;
		OnDownloadAndInstall(dummy);
	});
	m_installModBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnDownloadAndInstall, this);
	m_openWebBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnOpenPage, this);

	return panel;
}

wxPanel* GameBananaWindow::CreateInstalledModsTab(wxWindow* parent)
{
	auto* panel = new wxPanel(parent);
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	auto* topBar = new wxBoxSizer(wxHORIZONTAL);
	topBar->Add(new wxStaticText(panel, wxID_ANY,
								 _("Manage downloaded GameBanana mods and custom GraphicPacks installed in Cemu:")),
				1, wxALIGN_CENTER_VERTICAL | wxALL, 6);

	auto* importZipBtn = new wxButton(panel, wxID_ANY, _("Import Local Mod (.zip)..."));
	importZipBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnImportLocalZip, this);
	topBar->Add(importZipBtn, 0, wxALL, 4);

	auto* refreshBtn = new wxButton(panel, wxID_ANY, _("Refresh List"));
	refreshBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshInstalledMods(); });
	topBar->Add(refreshBtn, 0, wxALL, 4);

	sizer->Add(topBar, 0, wxEXPAND);

	m_installedList = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
	m_installedList->AppendColumn(_("Status"), wxLIST_FORMAT_LEFT, 100);
	m_installedList->AppendColumn(_("Mod / GraphicPack Name"), wxLIST_FORMAT_LEFT, 310);
	m_installedList->AppendColumn(_("Target Title IDs"), wxLIST_FORMAT_LEFT, 220);
	m_installedList->AppendColumn(_("Mod Contents"), wxLIST_FORMAT_LEFT, 180);
	m_installedList->AppendColumn(_("Install Path"), wxLIST_FORMAT_LEFT, 260);
	sizer->Add(m_installedList, 1, wxEXPAND | wxALL, 6);

	auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
	auto* toggleBtn = new wxButton(panel, wxID_ANY, _("Toggle Enable / Disable"));
	toggleBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnToggleInstalledMod, this);
	btnRow->Add(toggleBtn, 0, wxALL, 4);

	auto* openModDirBtn = new wxButton(panel, wxID_ANY, _("Open Selected Mod Folder"));
	openModDirBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnOpenSelectedModFolder, this);
	btnRow->Add(openModDirBtn, 0, wxALL, 4);

	auto* deleteModBtn = new wxButton(panel, wxID_ANY, _("Uninstall / Delete Mod"));
	deleteModBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnDeleteSelectedMod, this);
	btnRow->Add(deleteModBtn, 0, wxALL, 4);

	sizer->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
	panel->SetSizer(sizer);

	m_installedList->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
		wxCommandEvent dummy;
		OnToggleInstalledMod(dummy);
	});

	return panel;
}

wxPanel* GameBananaWindow::CreateCheatsTab(wxWindow* parent)
{
	auto* panel = new wxPanel(parent);
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	auto* intro = new wxStaticText(panel, wxID_ANY,
								   _("Create, customize, and activate Wii U cheat codes & assembly patches as native Cemu v7 GraphicPacks (patches.txt)."));
	sizer->Add(intro, 0, wxALL, 6);

	auto* formGrid = new wxFlexGridSizer(0, 2, 6, 10);
	formGrid->AddGrowableCol(1, 1);

	formGrid->Add(new wxStaticText(panel, wxID_ANY, _("Target Wii U Game:")), 0, wxALIGN_CENTER_VERTICAL);
	wxArrayString cheatGames;
	for (size_t i = 1; i < std::size(kWiiUGames); ++i)
		cheatGames.Add(wxString::FromUTF8(kWiiUGames[i].name));
	m_cheatGameChoice = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, cheatGames);
	int initialGameIdx = FindGameIndexByTitleId(m_initialTitleId, m_initialGameName);
	m_cheatGameChoice->SetSelection(initialGameIdx > 0 ? initialGameIdx - 1 : 0);
	formGrid->Add(m_cheatGameChoice, 1, wxEXPAND);

	formGrid->Add(new wxStaticText(panel, wxID_ANY, _("Built-in Cheat Template:")), 0, wxALIGN_CENTER_VERTICAL);
	m_cheatPresetChoice = new wxChoice(panel, wxID_ANY);
	formGrid->Add(m_cheatPresetChoice, 1, wxEXPAND);

	formGrid->Add(new wxStaticText(panel, wxID_ANY, _("Cheat Pack Name:")), 0, wxALIGN_CENTER_VERTICAL);
	m_cheatNameCtrl = new wxTextCtrl(panel, wxID_ANY);
	formGrid->Add(m_cheatNameCtrl, 1, wxEXPAND);

	formGrid->Add(new wxStaticText(panel, wxID_ANY, _("Target Title IDs (CSV):")), 0, wxALIGN_CENTER_VERTICAL);
	m_cheatTitleIdsCtrl = new wxTextCtrl(panel, wxID_ANY);
	formGrid->Add(m_cheatTitleIdsCtrl, 1, wxEXPAND);

	sizer->Add(formGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

	sizer->Add(new wxStaticText(panel, wxID_ANY, _("Cemu GraphicPack v7 patches.txt Code (PowerPC assembly / memory writes):")),
			   0, wxLEFT | wxRIGHT | wxTOP, 6);

	m_cheatCodeCtrl = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
									 wxTE_MULTILINE | wxHSCROLL | wxTE_RICH2);
	wxFont monoFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
	m_cheatCodeCtrl->SetFont(monoFont);
	sizer->Add(m_cheatCodeCtrl, 1, wxEXPAND | wxALL, 6);

	auto* actionRow = new wxBoxSizer(wxHORIZONTAL);
	auto* saveActivateBtn = new wxButton(panel, wxID_ANY, _("Save + Activate Cheat Pack"));
	saveActivateBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnSaveAndActivateCheat, this);
	actionRow->Add(saveActivateBtn, 0, wxALL, 4);

	auto* browseGbCheatsBtn = new wxButton(panel, wxID_ANY, _("Find More Cheats on GameBanana..."));
	browseGbCheatsBtn->Bind(wxEVT_BUTTON, &GameBananaWindow::OnSearchGameBananaCheats, this);
	actionRow->Add(browseGbCheatsBtn, 0, wxALL, 4);

	sizer->Add(actionRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
	panel->SetSizer(sizer);

	m_cheatGameChoice->Bind(wxEVT_CHOICE, &GameBananaWindow::OnCheatGameChanged, this);
	m_cheatPresetChoice->Bind(wxEVT_CHOICE, &GameBananaWindow::OnCheatPresetChanged, this);

	PopulateCheatPresetsForGame(m_cheatGameChoice->GetSelection() + 1);

	return panel;
}

void GameBananaWindow::UpdatePlaceholderBitmap()
{
	m_currentPreviewImage.Destroy();
	RenderPreviewBitmap();
}

void GameBananaWindow::RenderPreviewBitmap()
{
	if (!m_previewBitmap)
		return;

	const int w = std::max(160, m_previewTargetSize.GetWidth());
	const int h = std::max(90, m_previewTargetSize.GetHeight());

	if (m_currentPreviewImage.IsOk())
	{
		wxImage scaled = m_currentPreviewImage.Scale(w, h, wxIMAGE_QUALITY_HIGH);
		if (scaled.IsOk())
		{
			m_previewBitmap->SetBitmap(wxBitmap(scaled));
			return;
		}
	}

	wxBitmap bmp(w, h);
	wxMemoryDC dc(bmp);
	const bool dark = CemuApp::IsDarkMode();
	dc.SetBackground(wxBrush(dark ? wxColour(38, 38, 44) : wxColour(230, 232, 238)));
	dc.Clear();
	dc.SetPen(wxPen(dark ? wxColour(75, 75, 85) : wxColour(190, 195, 205), 1));
	dc.SetBrush(*wxTRANSPARENT_BRUSH);
	dc.DrawRectangle(0, 0, w, h);
	dc.SetTextForeground(dark ? wxColour(180, 185, 195) : wxColour(90, 95, 105));
	wxString label = _("GameBanana Preview");
	wxSize ext = dc.GetTextExtent(label);
	dc.DrawText(label, (w - ext.GetWidth()) / 2, (h - ext.GetHeight()) / 2);
	dc.SelectObject(wxNullBitmap);
	m_previewBitmap->SetBitmap(bmp);
}

void GameBananaWindow::UpdateResponsiveLayout()
{
	if (m_updatingLayout)
		return;
	m_updatingLayout = true;

	const int vscrollW = std::max(18, wxSystemSettings::GetMetric(wxSYS_VSCROLL_X));

	// 1. Safely resize m_list columns using outer widget width minus scrollbar + safety margin
	// so WS_HSCROLL never toggles on/off and never causes a Win32 WM_SIZE oscillation loop.
	if (m_list && m_list->GetColumnCount() >= 6)
	{
		const int availW = m_list->GetSize().GetWidth() - vscrollW - 24;
		if (availW > 360)
		{
			const int w0 = static_cast<int>(availW * 0.37);
			const int w1 = static_cast<int>(availW * 0.21);
			const int w2 = static_cast<int>(availW * 0.14);
			const int w3 = static_cast<int>(availW * 0.14);
			const int w4 = std::max(50, static_cast<int>(availW * 0.065));
			const int w5 = std::max(58, availW - (w0 + w1 + w2 + w3 + w4));
			if (std::abs(m_list->GetColumnWidth(0) - w0) >= 3 ||
				std::abs(m_list->GetColumnWidth(5) - w5) >= 3)
			{
				m_list->Freeze();
				m_list->SetColumnWidth(0, w0);
				m_list->SetColumnWidth(1, w1);
				m_list->SetColumnWidth(2, w2);
				m_list->SetColumnWidth(3, w3);
				m_list->SetColumnWidth(4, w4);
				m_list->SetColumnWidth(5, w5);
				m_list->Thaw();
			}
		}
	}

	// 2. Safely resize m_installedList columns
	if (m_installedList && m_installedList->GetColumnCount() >= 5)
	{
		const int availW = m_installedList->GetSize().GetWidth() - vscrollW - 24;
		if (availW > 420)
		{
			const int w0 = std::max(90, static_cast<int>(availW * 0.11));
			const int w1 = static_cast<int>(availW * 0.30);
			const int w2 = static_cast<int>(availW * 0.20);
			const int w3 = static_cast<int>(availW * 0.15);
			const int w4 = std::max(130, availW - (w0 + w1 + w2 + w3));
			if (std::abs(m_installedList->GetColumnWidth(1) - w1) >= 3 ||
				std::abs(m_installedList->GetColumnWidth(4) - w4) >= 3)
			{
				m_installedList->Freeze();
				m_installedList->SetColumnWidth(0, w0);
				m_installedList->SetColumnWidth(1, w1);
				m_installedList->SetColumnWidth(2, w2);
				m_installedList->SetColumnWidth(3, w3);
				m_installedList->SetColumnWidth(4, w4);
				m_installedList->Thaw();
			}
		}
	}

	// 3. Dynamically scale m_previewBitmap height based on dialog height so windowed mode
	// never clips the bottom buttons and maximized mode shows a large 440x248 preview.
	if (m_previewBitmap)
	{
		const int dlgH = GetClientSize().GetHeight();
		int targetH = std::clamp(dlgH - 410, 148, 248);
		int targetW = (targetH * 16) / 9;

		if (wxWindow* boxParent = m_previewBitmap->GetParent())
		{
			const int maxBoxW = boxParent->GetSize().GetWidth() - 28;
			if (maxBoxW >= 240 && targetW > maxBoxW)
			{
				targetW = maxBoxW;
				targetH = (targetW * 9) / 16;
			}
		}

		if (std::abs(targetW - m_previewTargetSize.GetWidth()) >= 6 ||
			std::abs(targetH - m_previewTargetSize.GetHeight()) >= 4)
		{
			m_previewTargetSize = wxSize(targetW, targetH);
			m_previewBitmap->SetMinSize(m_previewTargetSize);
			m_previewBitmap->SetSize(m_previewTargetSize);
			RenderPreviewBitmap();
			if (m_notebook && m_notebook->GetPageCount() > 0 && m_notebook->GetPage(0))
				m_notebook->GetPage(0)->Layout();
		}
	}

	m_updatingLayout = false;
}

void GameBananaWindow::SetStatus(const wxString& text)
{
	if (m_status)
		m_status->SetLabelText(text);
}

bool GameBananaWindow::MatchesSelectedCategory(const Entry& e) const
{
	const int catIdx = m_categoryChoice ? m_categoryChoice->GetSelection() : 0;
	if (catIdx <= 0 || catIdx >= static_cast<int>(std::size(kCategories)))
		return true;
	const std::string combined = boost::to_lower_copy(e.category + " " + e.rootCategory + " " + e.name);
	for (const auto& kw : kCategories[catIdx].keywords)
	{
		if (combined.find(kw) != std::string::npos)
			return true;
	}
	return false;
}

void GameBananaWindow::OnSearch(wxCommandEvent&)
{
	PerformSearch(1);
}

void GameBananaWindow::OnGameChanged(wxCommandEvent&)
{
	PerformSearch(1);
}

void GameBananaWindow::OnCategoryChanged(wxCommandEvent&)
{
	// Re-filter current results immediately; if none match and search was empty, trigger a targeted search
	ApplyClientFiltersAndShow();
	if (m_visibleEntries.empty() && !m_busy && m_categoryChoice->GetSelection() > 0)
	{
		PerformSearch(1);
	}
}

void GameBananaWindow::OnSortChanged(wxCommandEvent&)
{
	PerformSearch(1);
}

void GameBananaWindow::OnPrevPage(wxCommandEvent&)
{
	if (m_currentPage > 1 && !m_busy)
		PerformSearch(m_currentPage - 1);
}

void GameBananaWindow::OnNextPage(wxCommandEvent&)
{
	if (!m_busy)
		PerformSearch(m_currentPage + 1);
}

void GameBananaWindow::PerformSearch(int page)
{
	if (m_busy)
		return;

	m_currentPage = std::max(1, page);
	if (m_pageLabel)
		m_pageLabel->SetLabel(wxString::Format(_("Page %d"), m_currentPage));
	if (m_prevPageBtn)
		m_prevPageBtn->Enable(m_currentPage > 1);

	const int gameIdx = std::clamp(m_gameChoice->GetSelection(), 0, static_cast<int>(std::size(kWiiUGames) - 1));
	const int catIdx = std::clamp(m_categoryChoice->GetSelection(), 0, static_cast<int>(std::size(kCategories) - 1));
	const int sortIdx = std::clamp(m_sortChoice->GetSelection(), 0, static_cast<int>(std::size(kSortOrders) - 1));

	const uint64 gbGameId = kWiiUGames[gameIdx].gbGameId;
	const std::string selectedGameName = kWiiUGames[gameIdx].name;
	std::string query = m_search->GetValue().utf8_string();
	boost::trim(query);

	// If user picked a specific category and didn't enter a query, use category searchHint when searching
	if (query.empty() && catIdx > 0 && kCategories[catIdx].searchHint[0] != '\0')
	{
		query = kCategories[catIdx].searchHint;
	}

	std::string url;
	if (query.empty() && gbGameId != 0 && sortIdx == 0)
	{
		// Browse live mods for the selected Wii U game via Mod/Index
		url = fmt::format("https://gamebanana.com/apiv11/Mod/Index?_nPerpage=30&_nPage={}&_aFilters%5BGeneric_Game%5D={}",
						  m_currentPage, gbGameId);
	}
	else
	{
		if (query.empty())
			query = kWiiUGames[gameIdx].defaultSearch;
		url = fmt::format("https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Mod&_sOrder={}&_nPerpage=40&_nPage={}&_sSearchString={}",
						  kSortOrders[sortIdx].apiOrder, m_currentPage, UrlEscape(query));
		if (gbGameId != 0)
			url += fmt::format("&_idGameRow={}", gbGameId);
	}

	m_busy = true;
	SetStatus(wxString::Format(_("Fetching Wii U mods from GameBanana (Page %d)..."), m_currentPage));

	auto alive = m_alive;
	std::thread([this, alive, url, gbGameId, selectedGameName]() {
		std::string body;
		std::vector<Entry> parsed;
		bool ok = HttpGetString(url, body);
		if (ok)
		{
			rapidjson::Document doc;
			doc.Parse(body.c_str());
			if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("_aRecords") && doc["_aRecords"].IsArray())
			{
				for (const auto& r : doc["_aRecords"].GetArray())
				{
					if (!r.IsObject())
						continue;
					Entry e{};
					if (r.HasMember("_idRow") && r["_idRow"].IsUint64())
						e.id = r["_idRow"].GetUint64();
					if (r.HasMember("_sName") && r["_sName"].IsString())
						e.name = r["_sName"].GetString();
					if (r.HasMember("_sProfileUrl") && r["_sProfileUrl"].IsString())
						e.url = r["_sProfileUrl"].GetString();
					if (r.HasMember("_sVersion") && r["_sVersion"].IsString())
						e.version = r["_sVersion"].GetString();
					if (r.HasMember("_nLikeCount") && r["_nLikeCount"].IsUint64())
						e.likeCount = r["_nLikeCount"].GetUint64();
					if (r.HasMember("_nViewCount") && r["_nViewCount"].IsUint64())
						e.viewCount = r["_nViewCount"].GetUint64();

					if (r.HasMember("_aRootCategory") && r["_aRootCategory"].IsObject() &&
						r["_aRootCategory"].HasMember("_sName") && r["_aRootCategory"]["_sName"].IsString())
					{
						e.rootCategory = r["_aRootCategory"]["_sName"].GetString();
					}
					if (r.HasMember("_aCategory") && r["_aCategory"].IsObject() &&
						r["_aCategory"].HasMember("_sName") && r["_aCategory"]["_sName"].IsString())
					{
						e.category = r["_aCategory"]["_sName"].GetString();
					}
					if (e.category.empty())
						e.category = e.rootCategory.empty() ? "Mod" : e.rootCategory;

					if (r.HasMember("_aGame") && r["_aGame"].IsObject())
					{
						if (r["_aGame"].HasMember("_sName") && r["_aGame"]["_sName"].IsString())
							e.gameName = r["_aGame"]["_sName"].GetString();
						if (r["_aGame"].HasMember("_idRow") && r["_aGame"]["_idRow"].IsUint64())
							e.gameId = r["_aGame"]["_idRow"].GetUint64();
					}
					if (e.gameName.empty() && gbGameId != 0)
					{
						e.gameName = selectedGameName;
						e.gameId = gbGameId;
					}

					if (r.HasMember("_aSubmitter") && r["_aSubmitter"].IsObject() &&
						r["_aSubmitter"].HasMember("_sName") && r["_aSubmitter"]["_sName"].IsString())
					{
						e.author = r["_aSubmitter"]["_sName"].GetString();
					}

					e.thumbnailUrl = ExtractThumbnailUrl(r);

					if (e.id != 0)
						parsed.emplace_back(std::move(e));
				}
			}
			else
			{
				ok = false;
			}
		}

		wxTheApp->CallAfter([this, alive, ok, parsed = std::move(parsed)]() mutable {
			if (!*alive)
				return;
			m_busy = false;
			if (!ok)
			{
				SetStatus(_("Could not reach GameBanana API. Please check your internet connection."));
				return;
			}
			m_rawEntries = std::move(parsed);
			ApplyClientFiltersAndShow();
		});
	}).detach();
}

void GameBananaWindow::ApplyClientFiltersAndShow()
{
	m_visibleEntries.clear();
	m_list->DeleteAllItems();
	m_selectedVisibleIdx = -1;

	const wxColour baseBg = m_list->GetBackgroundColour();
	const wxColour altBg = wxHelper::CalculateAccentColour(baseBg);

	for (const auto& e : m_rawEntries)
	{
		if (!MatchesSelectedCategory(e))
			continue;
		const long idx = m_list->InsertItem(m_list->GetItemCount(), wxString::FromUTF8(e.name));
		m_list->SetItem(idx, 1, wxString::FromUTF8(e.gameName));
		m_list->SetItem(idx, 2, wxString::FromUTF8(e.category));
		m_list->SetItem(idx, 3, wxString::FromUTF8(e.author));
		m_list->SetItem(idx, 4, wxString::Format("%llu", static_cast<unsigned long long>(e.likeCount)));
		m_list->SetItem(idx, 5, wxString::Format("%llu", static_cast<unsigned long long>(e.viewCount)));
		m_list->SetItemBackgroundColour(idx, (idx % 2 == 0) ? baseBg : altBg);
		m_visibleEntries.push_back(e);
	}

	SetStatus(wxString::Format(_("Showing %zu mods in '%s' (Page %d). Select a mod to inspect files + install."),
							   m_visibleEntries.size(),
							   m_categoryChoice->GetStringSelection(),
							   m_currentPage));

	if (!m_visibleEntries.empty())
	{
		m_list->SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
	}
	UpdateResponsiveLayout();
}

void GameBananaWindow::OnModSelected(wxListEvent& event)
{
	const long idx = event.GetIndex();
	if (idx < 0 || idx >= static_cast<long>(m_visibleEntries.size()))
		return;

	m_selectedVisibleIdx = idx;
	const auto& entry = m_visibleEntries[idx];

	m_detailTitle->SetLabelText(wxString::FromUTF8(entry.name));
	m_detailMeta->SetLabelText(wxString::Format(_("Game: %s  |  Category: %s  |  Author: %s  |  Likes: %llu"),
												wxString::FromUTF8(entry.gameName),
												wxString::FromUTF8(entry.category),
												wxString::FromUTF8(entry.author),
												static_cast<unsigned long long>(entry.likeCount)));
	m_openWebBtn->Enable(!entry.url.empty());

	UpdatePlaceholderBitmap();
	FetchModDetailsAndThumbnail(static_cast<size_t>(idx));
}

void GameBananaWindow::FetchModDetailsAndThumbnail(size_t visibleIndex)
{
	if (visibleIndex >= m_visibleEntries.size())
		return;

	const Entry target = m_visibleEntries[visibleIndex];
	m_detailDesc->SetValue(_("Loading mod description and file list from GameBanana..."));
	m_fileChoice->Clear();
	m_fileChoice->Enable(false);
	m_installModBtn->Enable(false);

	auto alive = m_alive;
	std::thread([this, alive, target, visibleIndex]() {
		// 1. Fetch full Mod metadata + _aFiles + _sText
		std::string detailsBody;
		const std::string detailsUrl =
			fmt::format("https://gamebanana.com/apiv11/Mod/{}?_csvProperties=_sName,_sText,_sDescription,_aFiles,_nDownloadCount,_nLikeCount,_nViewCount,_sVersion,_aPreviewMedia",
						target.id);

		Entry updated = target;
		if (HttpGetString(detailsUrl, detailsBody))
		{
			rapidjson::Document doc;
			doc.Parse(detailsBody.c_str());
			if (!doc.HasParseError() && doc.IsObject())
			{
				std::string subDesc;
				if (doc.HasMember("_sDescription") && doc["_sDescription"].IsString())
					subDesc = doc["_sDescription"].GetString();
				std::string bodyHtml;
				if (doc.HasMember("_sText") && doc["_sText"].IsString())
					bodyHtml = StripHtmlTags(doc["_sText"].GetString());

				if (!subDesc.empty() && !bodyHtml.empty())
					updated.description = subDesc + "\n\n" + bodyHtml;
				else if (!bodyHtml.empty())
					updated.description = bodyHtml;
				else
					updated.description = subDesc;

				if (doc.HasMember("_nDownloadCount") && doc["_nDownloadCount"].IsUint64())
					updated.downloadCount = doc["_nDownloadCount"].GetUint64();
				if (doc.HasMember("_nLikeCount") && doc["_nLikeCount"].IsUint64())
					updated.likeCount = doc["_nLikeCount"].GetUint64();
				if (doc.HasMember("_nViewCount") && doc["_nViewCount"].IsUint64())
					updated.viewCount = doc["_nViewCount"].GetUint64();
				if (doc.HasMember("_sVersion") && doc["_sVersion"].IsString())
					updated.version = doc["_sVersion"].GetString();

				if (updated.thumbnailUrl.empty())
					updated.thumbnailUrl = ExtractThumbnailUrl(doc);

				if (doc.HasMember("_aFiles") && doc["_aFiles"].IsArray())
				{
					for (const auto& f : doc["_aFiles"].GetArray())
					{
						if (!f.IsObject())
							continue;
						FileEntry fe{};
						if (f.HasMember("_idRow") && f["_idRow"].IsUint64())
							fe.id = f["_idRow"].GetUint64();
						if (f.HasMember("_sFile") && f["_sFile"].IsString())
							fe.fileName = f["_sFile"].GetString();
						if (f.HasMember("_sDownloadUrl") && f["_sDownloadUrl"].IsString())
							fe.downloadUrl = f["_sDownloadUrl"].GetString();
						if (f.HasMember("_sDescription") && f["_sDescription"].IsString())
							fe.description = f["_sDescription"].GetString();
						if (f.HasMember("_nFilesize") && f["_nFilesize"].IsUint64())
							fe.fileSize = f["_nFilesize"].GetUint64();
						if (f.HasMember("_nDownloadCount") && f["_nDownloadCount"].IsUint64())
							fe.downloadCount = f["_nDownloadCount"].GetUint64();
						if (!fe.downloadUrl.empty())
							updated.files.push_back( std::move(fe));
					}
				}
				updated.detailsLoaded = true;
			}
		}

		// 2. Fetch preview image bytes if available
		std::vector<uint8> imgBytes;
		if (!updated.thumbnailUrl.empty())
		{
			HttpDownloadToBuffer(updated.thumbnailUrl, imgBytes);
		}

		wxTheApp->CallAfter([this, alive, visibleIndex, updated = std::move(updated), imgBytes = std::move(imgBytes)]() mutable {
			if (!*alive)
				return;
			if (visibleIndex >= m_visibleEntries.size() || m_visibleEntries[visibleIndex].id != updated.id)
				return;

			m_visibleEntries[visibleIndex] = updated;
			if (m_selectedVisibleIdx != static_cast<long>(visibleIndex))
				return;

			m_detailMeta->SetLabelText(wxString::Format(_("Game: %s  |  Category: %s  |  Author: %s  |  Likes: %llu  |  Downloads: %llu"),
														wxString::FromUTF8(updated.gameName),
														wxString::FromUTF8(updated.category),
														wxString::FromUTF8(updated.author),
														static_cast<unsigned long long>(updated.likeCount),
														static_cast<unsigned long long>(updated.downloadCount)));

			if (!updated.description.empty())
				m_detailDesc->SetValue(wxString::FromUTF8(updated.description));
			else
				m_detailDesc->SetValue(_("No description provided by author."));

			m_fileChoice->Clear();
			for (const auto& f : updated.files)
			{
				std::string label = fmt::format("{} ({}, {} DLs)", f.fileName, FormatByteSize(f.fileSize), f.downloadCount);
				if (!f.description.empty())
					label += " - " + f.description;
				m_fileChoice->Append(wxString::FromUTF8(label));
			}
			if (!updated.files.empty())
			{
				m_fileChoice->SetSelection(0);
				m_fileChoice->Enable(true);
				m_installModBtn->Enable(true);
			}
			else
			{
				m_fileChoice->Append(_("No direct download files listed (use 'Open Web Page')"));
				m_fileChoice->SetSelection(0);
				m_fileChoice->Enable(false);
				m_installModBtn->Enable(false);
			}

			if (!imgBytes.empty())
			{
				wxLogNull noLog;
				wxMemoryInputStream stream(imgBytes.data(), imgBytes.size());
				wxImage img;
				if (img.LoadFile(stream) && img.IsOk())
				{
					m_currentPreviewImage = img;
					RenderPreviewBitmap();
				}
			}
		});
	}).detach();
}

void GameBananaWindow::OnOpenPage(wxCommandEvent&)
{
	if (m_selectedVisibleIdx < 0 || m_selectedVisibleIdx >= static_cast<long>(m_visibleEntries.size()))
		return;
	const auto& url = m_visibleEntries[m_selectedVisibleIdx].url;
	if (!url.empty())
		wxLaunchDefaultBrowser(wxString::FromUTF8(url));
}

void GameBananaWindow::OnOpenModsFolder(wxCommandEvent&)
{
	const fs::path dir = ActiveSettings::GetUserDataPath("graphicPacks/downloadedMods");
	std::error_code ec;
	fs::create_directories(dir, ec);
	wxLaunchDefaultApplication(wxHelper::FromPath(dir));
}

void GameBananaWindow::OnDownloadAndInstall(wxCommandEvent&)
{
	if (m_busy || m_selectedVisibleIdx < 0 || m_selectedVisibleIdx >= static_cast<long>(m_visibleEntries.size()))
		return;

	const Entry entry = m_visibleEntries[m_selectedVisibleIdx];
	if (entry.files.empty())
	{
		wxMessageBox(_("Select a mod with downloadable files first, or click 'Open on GameBanana'."),
					 _("GameBanana Mod Manager"), wxOK | wxICON_INFORMATION, this);
		return;
	}

	const int fileIdx = std::clamp(m_fileChoice->GetSelection(), 0, static_cast<int>(entry.files.size() - 1));
	const FileEntry fileEntry = entry.files[fileIdx];

	const int gameIdx = FindGameIndexByGbGameId(entry.gameId, entry.gameName);
	const std::string fallbackTitleIds = kWiiUGames[gameIdx].titleIdsCsv;

	m_busy = true;
	m_installModBtn->Enable(false);
	m_downloadGauge->SetValue(0);
	SetStatus(wxString::Format(_("Downloading '%s' (%s)..."),
							   wxString::FromUTF8(fileEntry.fileName),
							   wxString::FromUTF8(FormatByteSize(fileEntry.fileSize))));

	auto alive = m_alive;
	std::thread([this, alive, entry, fileEntry, fallbackTitleIds]() {
		std::vector<uint8> fileBytes;
		const bool dlOk = HttpDownloadToBuffer(fileEntry.downloadUrl, fileBytes, [this, alive](int pct) {
			wxTheApp->CallAfter([this, alive, pct]() {
				if (!*alive)
					return;
				m_downloadGauge->SetValue(pct);
			});
		});

		fs::path installedPath;
		std::string errMsg;
		bool installedAsGraphicPack = false;

		if (dlOk)
		{
			// Always save raw archive into user's gamebanana folder as a backup or if it's a .7z/.rar
			std::string safeFile = fileEntry.fileName;
			boost::replace_all(safeFile, "\\", "/");
			safeFile = _pathToUtf8(_utf8ToPath(safeFile).filename());
			if (safeFile.empty())
				safeFile = std::to_string(entry.id) + ".zip";

			const fs::path archiveDir = ActiveSettings::GetUserDataPath("gamebanana") / SanitizeFolderName(entry.gameName);
			std::error_code ec;
			fs::create_directories(archiveDir, ec);
			const fs::path archiveTarget = archiveDir / _utf8ToPath(safeFile);
			if (std::unique_ptr<FileStream> fsOut{FileStream::createFile2(archiveTarget)})
			{
				fsOut->writeData(fileBytes.data(), fileBytes.size());
			}

			// Check if it's a ZIP archive (PK\x03\x04 magic or .zip extension)
			const bool isZipMagic = fileBytes.size() >= 4 &&
									fileBytes[0] == 'P' && fileBytes[1] == 'K' &&
									fileBytes[2] == 0x03 && fileBytes[3] == 0x04;
			if (isZipMagic || boost::iends_with(safeFile, ".zip"))
			{
				installedAsGraphicPack = InstallZipAsGraphicPack(
					fileBytes, entry.name, entry.author, entry.category,
					entry.version.empty() ? "1.0" : entry.version,
					entry.gameName, fallbackTitleIds, installedPath, errMsg);
			}
			else
			{
				installedPath = archiveTarget;
				errMsg = "Saved archive to 'gamebanana' folder (non-ZIP archive format: extract manually if needed).";
			}
		}

		wxTheApp->CallAfter([this, alive, dlOk, installedAsGraphicPack, installedPath, errMsg, entry]() {
			if (!*alive)
				return;
			m_busy = false;
			m_installModBtn->Enable(true);
			m_downloadGauge->SetValue(dlOk ? 100 : 0);

			if (!dlOk)
			{
				SetStatus(_("Download failed. Try 'Open on GameBanana' to download via browser."));
				wxMessageBox(_("Failed to download file from GameBanana."), _("Download Error"), wxOK | wxICON_ERROR, this);
				return;
			}

			if (installedAsGraphicPack)
			{
				EnableAndReloadGraphicPack(installedPath);
				RefreshInstalledMods();
				SetStatus(wxString::Format(_("Installed & enabled '%s' in GraphicPacks (%s)"),
										   wxString::FromUTF8(entry.name),
										   wxHelper::FromPath(installedPath)));
				wxMessageBox(wxString::Format(_("Successfully downloaded and installed '%s'!\n\n"
												"Location:\n%s\n\n"
												"The mod has been automatically enabled in Cemu's GraphicPacks."),
											  wxString::FromUTF8(entry.name),
											  wxHelper::FromPath(installedPath)),
							 _("GameBanana Mod Installed"), wxOK | wxICON_INFORMATION, this);
			}
			else
			{
				RefreshInstalledMods();
				SetStatus(wxString::Format(_("Saved archive to %s (%s)"),
										   wxHelper::FromPath(installedPath),
										   wxString::FromUTF8(errMsg)));
				wxMessageBox(wxString::Format(_("Downloaded '%s' to:\n%s\n\nNote: %s"),
											  wxString::FromUTF8(entry.name),
											  wxHelper::FromPath(installedPath),
											  wxString::FromUTF8(errMsg)),
							 _("GameBanana Download Complete"), wxOK | wxICON_INFORMATION, this);
			}
		});
	}).detach();
}

bool GameBananaWindow::InstallZipAsGraphicPack(const std::vector<uint8>& zipBytes,
											   const std::string& modName,
											   const std::string& modAuthor,
											   const std::string& modCategory,
											   const std::string& modVersion,
											   const std::string& gameName,
											   const std::string& fallbackTitleIds,
											   fs::path& outInstalledDir,
											   std::string& outError)
{
	zip_error_t error;
	zip_error_init(&error);

	zip_source_t* src = zip_source_buffer_create(zipBytes.data(), zipBytes.size(), 0, &error);
	if (!src)
	{
		outError = "Failed to initialize ZIP buffer.";
		zip_error_fini(&error);
		return false;
	}

	zip_t* za = zip_open_from_source(src, 0, &error);
	if (!za)
	{
		zip_source_free(src);
		zip_error_fini(&error);
		outError = "Archive is not a valid ZIP file.";
		return false;
	}

	const std::string folderSlug = SanitizeFolderName(gameName + "_" + modName);
	const fs::path targetRoot = ActiveSettings::GetUserDataPath("graphicPacks/downloadedMods") / _utf8ToPath(folderSlug);
	std::error_code ec;
	fs::create_directories(targetRoot, ec);

	bool hasRulesTxt = false;
	const zip_int64_t numEntries = zip_get_num_entries(za, 0);
	for (zip_int64_t i = 0; i < numEntries; ++i)
	{
		zip_stat_t sb{};
		if (zip_stat_index(za, static_cast<zip_uint64_t>(i), 0, &sb) != 0 || !sb.name)
			continue;

		std::string entryName = sb.name;
		boost::replace_all(entryName, "\\", "/");
		if (entryName.empty() || entryName.find("../") != std::string::npos || entryName.front() == '/')
			continue;

		const fs::path destPath = targetRoot / _utf8ToPath(entryName);
		if (entryName.back() == '/')
		{
			fs::create_directories(destPath, ec);
			continue;
		}

		if (boost::iequals(_pathToUtf8(destPath.filename()), "rules.txt"))
			hasRulesTxt = true;

		if (sb.size > 512ULL * 1024ULL * 1024ULL)
			continue;

		fs::create_directories(destPath.parent_path(), ec);

		zip_file_t* zf = zip_fopen_index(za, static_cast<zip_uint64_t>(i), 0);
		if (!zf)
			continue;

		std::vector<uint8> buf(static_cast<size_t>(sb.size));
		if (sb.size == 0 || zip_fread(zf, buf.data(), sb.size) == static_cast<zip_int64_t>(sb.size))
		{
			if (std::unique_ptr<FileStream> fsOut{FileStream::createFile2(destPath)})
			{
				if (!buf.empty())
					fsOut->writeData(buf.data(), buf.size());
			}
		}
		zip_fclose(zf);
	}

	zip_close(za);
	zip_error_fini(&error);

	// If the ZIP did not include a Cemu rules.txt, locate where content/ or aoc/ lives (or targetRoot) and generate rules.txt
	fs::path packRoot = targetRoot;
	if (!hasRulesTxt)
	{
		for (fs::recursive_directory_iterator it(targetRoot, ec); it != fs::recursive_directory_iterator(); ++it)
		{
			if (it->is_directory(ec))
			{
				const std::string dirName = _pathToUtf8(it->path().filename());
				if (boost::iequals(dirName, "content") || boost::iequals(dirName, "aoc"))
				{
					packRoot = it->path().parent_path();
					break;
				}
			}
		}

		std::string cleanModName = modName;
		boost::replace_all(cleanModName, "\"", "'");
		std::string cleanGameName = gameName.empty() ? "Wii U" : gameName;
		boost::replace_all(cleanGameName, "\"", "'");
		std::string cleanCat = modCategory.empty() ? "Mods" : modCategory;
		boost::replace_all(cleanCat, "\"", "'");

		std::ostringstream rules;
		rules << "[Definition]\n";
		rules << "titleIds = " << fallbackTitleIds << "\n";
		rules << "name = \"" << cleanModName << "\"\n";
		rules << "path = \"" << cleanGameName << "/GameBanana Mods/" << cleanCat << "/" << cleanModName << "\"\n";
		rules << "description = \"GameBanana Mod by " << (modAuthor.empty() ? "Community" : modAuthor)
			  << " (v" << modVersion << "). Installed via Cemu GameBanana Mod Manager.\"\n";
		rules << "version = 7\n";

		const fs::path rulesFile = packRoot / "rules.txt";
		if (std::unique_ptr<FileStream> fsRules{FileStream::createFile2(rulesFile)})
		{
			fsRules->writeString(rules.str().c_str());
		}
	}

	outInstalledDir = packRoot;
	return true;
}

void GameBananaWindow::EnableAndReloadGraphicPack(const fs::path& installedDir)
{
	// Preserve currently enabled GraphicPacks in config before reloading
	auto& entries = g_config.data().graphic_pack_entries;
	for (const auto& gp : GraphicPack2::GetGraphicPacks())
	{
		const auto normPath = _utf8ToPath(gp->GetNormalizedPathString());
		if (gp->IsEnabled())
		{
			entries.try_emplace(normPath);
		}
	}

	// Reload all GraphicPacks so the newly installed mod is discovered
	if (!CafeSystem::IsTitleRunning())
	{
		GraphicPack2::ClearGraphicPacks();
		GraphicPack2::LoadAll();
	}
	else
	{
		GraphicPack2::LoadGraphicPack(installedDir);
	}

	// Find and enable any GraphicPack inside installedDir
	for (const auto& gp : GraphicPack2::GetGraphicPacks())
	{
		const std::string rulesPathStr = _pathToUtf8(gp->GetRulesPath());
		const std::string installedStr = _pathToUtf8(installedDir);
		if (boost::icontains(rulesPathStr, installedStr))
		{
			gp->SetEnabled(true);
			const auto normPath = _utf8ToPath(gp->GetNormalizedPathString());
			entries[normPath].erase("_disabled");
			if (CafeSystem::IsTitleRunning() && gp->ContainsTitleId(CafeSystem::GetForegroundTitleId()))
			{
				GraphicPack2::ActivateGraphicPack(gp);
			}
		}
	}
	g_config.Save();
}

void GameBananaWindow::RefreshInstalledMods()
{
	if (!m_installedList)
		return;

	m_installedMods.clear();
	m_installedList->DeleteAllItems();

	const fs::path modsBase = ActiveSettings::GetUserDataPath("graphicPacks/downloadedMods");
	std::error_code ec;
	fs::create_directories(modsBase, ec);

	const wxColour baseBg = m_installedList->GetBackgroundColour();
	const wxColour altBg = wxHelper::CalculateAccentColour(baseBg);

	for (const auto& gp : GraphicPack2::GetGraphicPacks())
	{
		const fs::path rulesPath = gp->GetRulesPath();
		const std::string normStr = gp->GetNormalizedPathString();
		if (!boost::icontains(normStr, "downloadedMods") && !boost::icontains(gp->GetVirtualPath(), "GameBanana"))
			continue;

		InstalledModInfo info{};
		info.name = gp->GetName();
		info.folderPath = rulesPath.parent_path();
		info.rulesPath = rulesPath;
		info.normalizedPath = normStr;
		info.isGraphicPack = true;
		info.enabled = gp->IsEnabled();

		std::ostringstream tids;
		for (size_t i = 0; i < gp->GetTitleIds().size() && i < 3; ++i)
		{
			if (i > 0)
				tids << ", ";
			tids << fmt::format("{:016x}", gp->GetTitleIds()[i]);
		}
		if (gp->GetTitleIds().size() > 3)
			tids << "...";
		info.gameTitleIds = tids.str();

		std::vector<std::string> contents;
		if (fs::exists(info.folderPath / "content", ec) || fs::exists(info.folderPath / "aoc", ec))
			contents.emplace_back("3D Models / Textures / Content");
		if (fs::exists(info.folderPath / "patches.txt", ec))
			contents.emplace_back("Cheats / Code Patches");
		if (gp->HasShaders())
			contents.emplace_back("Shaders");
		if (!gp->GetTextureRules().empty())
			contents.emplace_back("Texture Rules");
		if (contents.empty())
			contents.emplace_back("GraphicPack Mod");
		info.contentType = boost::algorithm::join(contents, " + ");

		const long idx = m_installedList->InsertItem(m_installedList->GetItemCount(),
													 info.enabled ? _("Enabled") : _("Disabled"));
		m_installedList->SetItem(idx, 1, wxString::FromUTF8(info.name));
		m_installedList->SetItem(idx, 2, wxString::FromUTF8(info.gameTitleIds));
		m_installedList->SetItem(idx, 3, wxString::FromUTF8(info.contentType));
		m_installedList->SetItem(idx, 4, wxHelper::FromPath(info.folderPath));
		m_installedList->SetItemBackgroundColour(idx, (idx % 2 == 0) ? baseBg : altBg);
		if (info.enabled)
		{
			m_installedList->SetItemTextColour(
				idx, CemuApp::IsDarkMode() ? wxColour(76, 224, 122) : wxColour(0, 140, 40));
		}
		m_installedMods.push_back(std::move(info));
	}
}

void GameBananaWindow::OnToggleInstalledMod(wxCommandEvent&)
{
	const long sel = m_installedList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (sel < 0 || sel >= static_cast<long>(m_installedMods.size()))
		return;

	const auto& mod = m_installedMods[sel];
	for (const auto& gp : GraphicPack2::GetGraphicPacks())
	{
		if (gp->GetNormalizedPathString() == mod.normalizedPath)
		{
			const bool newState = !gp->IsEnabled();
			gp->SetEnabled(newState);

			auto& entries = g_config.data().graphic_pack_entries;
			const auto key = _utf8ToPath(gp->GetNormalizedPathString());
			if (newState)
			{
				entries.try_emplace(key);
				entries[key].erase("_disabled");
			}
			else
			{
				entries.erase(key);
			}
			g_config.Save();
			break;
		}
	}
	RefreshInstalledMods();
}

void GameBananaWindow::OnImportLocalZip(wxCommandEvent&)
{
	wxFileDialog openFileDialog(this, _("Import GameBanana Mod Archive (.zip)"), wxEmptyString, wxEmptyString,
								_("ZIP Mod Archives (*.zip)|*.zip"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (openFileDialog.ShowModal() != wxID_OK)
		return;

	const fs::path zipPath = _utf8ToPath(openFileDialog.GetPath().utf8_string());
	std::unique_ptr<FileStream> fsIn(FileStream::openFile2(zipPath));
	if (!fsIn)
	{
		wxMessageBox(_("Could not open the selected ZIP file."), _("Import Error"), wxOK | wxICON_ERROR, this);
		return;
	}

	std::vector<uint8> zipBytes;
	fsIn->extract(zipBytes);

	const int gameIdx = std::clamp(m_gameChoice->GetSelection(), 1, static_cast<int>(std::size(kWiiUGames) - 1));
	const std::string modName = _pathToUtf8(zipPath.stem());

	fs::path installedDir;
	std::string errMsg;
	if (InstallZipAsGraphicPack(zipBytes, modName, "Local Import", "Custom Mod", "1.0",
								kWiiUGames[gameIdx].name, kWiiUGames[gameIdx].titleIdsCsv,
								installedDir, errMsg))
	{
		EnableAndReloadGraphicPack(installedDir);
		RefreshInstalledMods();
		SetStatus(wxString::Format(_("Imported and enabled '%s'"), wxString::FromUTF8(modName)));
	}
	else
	{
		wxMessageBox(wxString::FromUTF8(errMsg), _("Import Error"), wxOK | wxICON_ERROR, this);
	}
}

void GameBananaWindow::OnOpenSelectedModFolder(wxCommandEvent&)
{
	const long sel = m_installedList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (sel < 0 || sel >= static_cast<long>(m_installedMods.size()))
		return;
	wxLaunchDefaultApplication(wxHelper::FromPath(m_installedMods[sel].folderPath));
}

void GameBananaWindow::OnDeleteSelectedMod(wxCommandEvent&)
{
	const long sel = m_installedList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (sel < 0 || sel >= static_cast<long>(m_installedMods.size()))
		return;

	const auto mod = m_installedMods[sel];
	if (wxMessageBox(wxString::Format(_("Are you sure you want to uninstall and delete '%s'?\n\n%s"),
									  wxString::FromUTF8(mod.name),
									  wxHelper::FromPath(mod.folderPath)),
					 _("Confirm Uninstall Mod"), wxYES_NO | wxICON_WARNING, this) != wxYES)
	{
		return;
	}

	g_config.data().graphic_pack_entries.erase(_utf8ToPath(mod.normalizedPath));
	g_config.Save();

	std::error_code ec;
	fs::remove_all(mod.folderPath, ec);

	if (!CafeSystem::IsTitleRunning())
	{
		GraphicPack2::ClearGraphicPacks();
		GraphicPack2::LoadAll();
	}
	RefreshInstalledMods();
	SetStatus(wxString::Format(_("Uninstalled mod '%s'."), wxString::FromUTF8(mod.name)));
}

void GameBananaWindow::PopulateCheatPresetsForGame(int gameIdx)
{
	if (!m_cheatPresetChoice)
		return;

	m_cheatPresetChoice->Clear();
	int firstMatchPreset = -1;
	for (size_t i = 0; i < std::size(kCheatTemplates); ++i)
	{
		const auto& ct = kCheatTemplates[i];
		if (ct.gameIdx == gameIdx || ct.gameIdx == 0)
		{
			const int itemIdx = m_cheatPresetChoice->Append(wxString::FromUTF8(ct.name), reinterpret_cast<void*>(i));
			if (firstMatchPreset == -1)
				firstMatchPreset = itemIdx;
		}
	}

	if (gameIdx >= 1 && gameIdx < static_cast<int>(std::size(kWiiUGames)))
		m_cheatTitleIdsCtrl->SetValue(wxString::FromUTF8(kWiiUGames[gameIdx].titleIdsCsv));

	if (firstMatchPreset != -1)
	{
		m_cheatPresetChoice->SetSelection(firstMatchPreset);
		wxCommandEvent dummy;
		OnCheatPresetChanged(dummy);
	}
}

void GameBananaWindow::OnCheatGameChanged(wxCommandEvent&)
{
	const int gameIdx = m_cheatGameChoice->GetSelection() + 1;
	PopulateCheatPresetsForGame(gameIdx);
}

void GameBananaWindow::OnCheatPresetChanged(wxCommandEvent&)
{
	const int sel = m_cheatPresetChoice->GetSelection();
	if (sel < 0)
		return;
	const auto presetIdx = reinterpret_cast<size_t>(m_cheatPresetChoice->GetClientData(sel));
	if (presetIdx >= std::size(kCheatTemplates))
		return;

	const auto& ct = kCheatTemplates[presetIdx];
	m_cheatNameCtrl->SetValue(wxString::FromUTF8(ct.name));
	m_cheatCodeCtrl->SetValue(wxString::FromUTF8(std::string("; ") + ct.description + "\n" + ct.patchesTxt));
}

void GameBananaWindow::OnSaveAndActivateCheat(wxCommandEvent&)
{
	std::string cheatName = m_cheatNameCtrl->GetValue().utf8_string();
	std::string titleIds = m_cheatTitleIdsCtrl->GetValue().utf8_string();
	std::string patchesCode = m_cheatCodeCtrl->GetValue().utf8_string();
	boost::trim(cheatName);
	boost::trim(titleIds);
	boost::trim(patchesCode);

	if (cheatName.empty() || titleIds.empty() || patchesCode.empty())
	{
		wxMessageBox(_("Please provide a Cheat Pack Name, Target Title IDs, and patches.txt code."),
					 _("Cheats Studio"), wxOK | wxICON_WARNING, this);
		return;
	}

	const int gameIdx = std::clamp(m_cheatGameChoice->GetSelection() + 1, 1, static_cast<int>(std::size(kWiiUGames) - 1));
	const std::string gameName = kWiiUGames[gameIdx].name;

	const std::string slug = "Cheat_" + SanitizeFolderName(cheatName);
	const fs::path packDir = ActiveSettings::GetUserDataPath("graphicPacks/downloadedMods") / _utf8ToPath(slug);
	std::error_code ec;
	fs::create_directories(packDir, ec);

	std::string cleanCheatName = cheatName;
	boost::replace_all(cleanCheatName, "\"", "'");

	std::ostringstream rules;
	rules << "[Definition]\n";
	rules << "titleIds = " << titleIds << "\n";
	rules << "name = \"" << cleanCheatName << "\"\n";
	rules << "path = \"" << gameName << "/GameBanana Mods/Cheats & Codes/" << cleanCheatName << "\"\n";
	rules << "description = \"Custom Cheat / Code Patch created in Cemu Cheats Studio.\"\n";
	rules << "version = 7\n";

	if (std::unique_ptr<FileStream> fsRules{FileStream::createFile2(packDir / "rules.txt")})
		fsRules->writeString(rules.str().c_str());

	if (std::unique_ptr<FileStream> fsPatches{FileStream::createFile2(packDir / "patches.txt")})
		fsPatches->writeString(patchesCode.c_str());

	EnableAndReloadGraphicPack(packDir);
	RefreshInstalledMods();

	SetStatus(wxString::Format(_("Saved and activated cheat pack '%s'!"), wxString::FromUTF8(cheatName)));
	wxMessageBox(wxString::Format(_("Cheat pack '%s' has been saved to:\n%s\n\nand enabled in Cemu GraphicPacks!"),
								  wxString::FromUTF8(cheatName),
								  wxHelper::FromPath(packDir)),
				 _("Cheat Pack Activated"), wxOK | wxICON_INFORMATION, this);
}

void GameBananaWindow::OnSearchGameBananaCheats(wxCommandEvent&)
{
	const int gameIdx = m_cheatGameChoice->GetSelection() + 1;
	m_gameChoice->SetSelection(gameIdx);
	m_categoryChoice->SetSelection(5); // "Cheats, Codes & Patches"
	m_search->SetValue(wxEmptyString);
	m_notebook->SetSelection(0);
	PerformSearch(1);
}
