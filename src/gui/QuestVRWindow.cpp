#include "gui/wxgui.h"
#include "gui/QuestVRWindow.h"
#include "gui/CemuApp.h"
#include "gui/MainWindow.h"
#include "gui/input/InputSettings2.h"
#include "input/InputManager.h"
#include "input/emulated/WPADController.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"

#include <wx/clipbrd.h>
#include <wx/msgdlg.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/utils.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if BOOST_OS_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objidl.h>
#include <gdiplus.h>
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif
#endif

namespace
{
	constexpr int kQuestVRTimerId = 29450;

	// Target source modes:
	// 0 = Player 1 (Main TV Display)
	// 1 = Player 2 (Separate GamePad View / DRC)
	// 2 = Player 1 (Left Half of Split-Screen)
	// 3 = Player 2 (Right Half of Split-Screen)
	struct QuestBroadcastService
	{
		std::atomic<bool> running{false};
		std::atomic<uintptr_t> mainHwnd{0};
		std::atomic<uintptr_t> padHwnd{0};
		std::atomic<int> targetSource{0};
		std::atomic<int> defaultXrMode{0}; // 0 = Mixed Reality (immersive-ar), 1 = Full-Screen VR (immersive-vr)
		std::atomic<int> targetFps{60};
		std::atomic<int> jpegQuality{85};
		std::atomic<uint16_t> port{8765};
		std::atomic<uint64_t> totalFramesCaptured{0};
		std::atomic<uint64_t> totalFramesServed{0};
		std::atomic<int> lastCaptureW{0};
		std::atomic<int> lastCaptureH{0};
		std::atomic<bool> adbForwardActive{false};

		struct QuestInputState
		{
			std::atomic<uint32_t> buttons{0};
			std::atomic<float> stickLX{0.0f};
			std::atomic<float> stickLY{0.0f};
			std::atomic<float> stickRX{0.0f};
			std::atomic<float> stickRY{0.0f};
			std::atomic<uint64_t> lastTimestampMs{0};
		};

		QuestInputState questInput[2]{};

		std::mutex frameMutex;
		std::vector<uint8_t> latestJpeg;
		uint64_t latestSeq{0};

		std::thread captureThread;
		std::thread serverThread;
#if BOOST_OS_WINDOWS
		std::atomic<uintptr_t> listenSock{static_cast<uintptr_t>(INVALID_SOCKET)};
#endif

		static QuestBroadcastService& Instance()
		{
			static QuestBroadcastService s_instance;
			return s_instance;
		}

		std::string DetectLanIp()
		{
#if BOOST_OS_WINDOWS
			WSADATA wsa{};
			WSAStartup(MAKEWORD(2, 2), &wsa);

			SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (s != INVALID_SOCKET)
			{
				sockaddr_in remote{};
				remote.sin_family = AF_INET;
				remote.sin_port = htons(53);
				inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
				if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0)
				{
					sockaddr_in local{};
					int len = sizeof(local);
					if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
					{
						char buf[64]{};
						if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)))
						{
							closesocket(s);
							std::string ip(buf);
							if (!ip.empty() && ip != "0.0.0.0" && ip != "127.0.0.1")
								return ip;
						}
					}
				}
				closesocket(s);
			}

			char host[256]{};
			if (gethostname(host, sizeof(host)) == 0)
			{
				addrinfo hints{};
				hints.ai_family = AF_INET;
				addrinfo* res = nullptr;
				if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res)
				{
					for (addrinfo* p = res; p != nullptr; p = p->ai_next)
					{
						auto* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
						char buf[64]{};
						if (inet_ntop(AF_INET, &ipv4->sin_addr, buf, sizeof(buf)))
						{
							std::string ip(buf);
							if (ip != "127.0.0.1" && ip.rfind("169.254.", 0) != 0)
							{
								freeaddrinfo(res);
								return ip;
							}
						}
					}
					freeaddrinfo(res);
				}
			}
#endif
			return "127.0.0.1";
		}

		std::string GetBroadcastUrl()
		{
			if (adbForwardActive.load())
				return fmt::format("http://localhost:{}", port.load());
			return fmt::format("http://{}:{}", DetectLanIp(), port.load());
		}

		void ParseAndApplyQuestInput(const std::string& req)
		{
			auto qPos = req.find('?');
			if (qPos == std::string::npos)
				return;
			auto lineEnd = req.find("\r\n", qPos);
			std::string query = (lineEnd != std::string::npos) ? req.substr(qPos + 1, lineEnd - qPos - 1) : req.substr(qPos + 1);
			auto spPos = query.find(' ');
			if (spPos != std::string::npos)
				query = query.substr(0, spPos);

			int p = (targetSource.load() == 1 || targetSource.load() == 3) ? 1 : 0;
			uint32_t btn = 0;
			float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
			bool hasInput = false;

			std::stringstream ss(query);
			std::string pair;
			while (std::getline(ss, pair, '&'))
			{
				auto eq = pair.find('=');
				if (eq == std::string::npos)
					continue;
				std::string k = pair.substr(0, eq);
				std::string v = pair.substr(eq + 1);
				if (k == "p")
					p = std::clamp(std::atoi(v.c_str()), 0, 1);
				else if (k == "btn")
				{
					btn = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
					hasInput = true;
				}
				else if (k == "lx")
				{
					lx = std::strtof(v.c_str(), nullptr);
					hasInput = true;
				}
				else if (k == "ly")
				{
					ly = std::strtof(v.c_str(), nullptr);
					hasInput = true;
				}
				else if (k == "rx")
				{
					rx = std::strtof(v.c_str(), nullptr);
					hasInput = true;
				}
				else if (k == "ry")
				{
					ry = std::strtof(v.c_str(), nullptr);
					hasInput = true;
				}
			}

			if (hasInput && p >= 0 && p < 2)
			{
				uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				questInput[p].buttons.store(btn);
				questInput[p].stickLX.store(std::clamp(lx, -1.0f, 1.0f));
				questInput[p].stickLY.store(std::clamp(ly, -1.0f, 1.0f));
				questInput[p].stickRX.store(std::clamp(rx, -1.0f, 1.0f));
				questInput[p].stickRY.store(std::clamp(ry, -1.0f, 1.0f));
				questInput[p].lastTimestampMs.store(nowMs);
			}
		}

#if BOOST_OS_WINDOWS
		static bool GetJpegEncoderClsid(CLSID& outClsid)
		{
			static bool s_cached = false;
			static CLSID s_clsid{};
			if (s_cached)
			{
				outClsid = s_clsid;
				return true;
			}
			UINT num = 0, size = 0;
			Gdiplus::GetImageEncodersSize(&num, &size);
			if (size == 0)
				return false;
			std::vector<uint8_t> buf(size);
			auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
			Gdiplus::GetImageEncoders(num, size, codecs);
			for (UINT i = 0; i < num; ++i)
			{
				if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0)
				{
					s_clsid = codecs[i].Clsid;
					s_cached = true;
					outClsid = s_clsid;
					return true;
				}
			}
			return false;
		}

		void CaptureLoop()
		{
			ULONG_PTR gdiplusToken = 0;
			Gdiplus::GdiplusStartupInput gdiplusStartupInput;
			Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

			CLSID clsidJpeg{};
			if (!GetJpegEncoderClsid(clsidJpeg))
			{
				Gdiplus::GdiplusShutdown(gdiplusToken);
				return;
			}

			while (running.load())
			{
				const auto frameStart = std::chrono::steady_clock::now();

				const int srcMode = targetSource.load();
				HWND hwndTarget = nullptr;
				if (srcMode == 1)
				{
					hwndTarget = reinterpret_cast<HWND>(padHwnd.load());
					if (!hwndTarget || !IsWindow(hwndTarget))
						hwndTarget = reinterpret_cast<HWND>(mainHwnd.load());
				}
				else
				{
					hwndTarget = reinterpret_cast<HWND>(mainHwnd.load());
				}

				if (hwndTarget && IsWindow(hwndTarget))
				{
					RECT rc{};
					GetClientRect(hwndTarget, &rc);
					const int clientW = rc.right - rc.left;
					const int clientH = rc.bottom - rc.top;

					if (clientW > 64 && clientH > 64)
					{
						HDC hdcWin = GetDC(hwndTarget);
						if (hdcWin)
						{
							HDC hdcFull = CreateCompatibleDC(hdcWin);
							HBITMAP hbmFull = CreateCompatibleBitmap(hdcWin, clientW, clientH);
							HGDIOBJ oldFull = SelectObject(hdcFull, hbmFull);

							BOOL prOk = PrintWindow(hwndTarget, hdcFull, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
							if (!prOk)
								BitBlt(hdcFull, 0, 0, clientW, clientH, hdcWin, 0, 0, SRCCOPY);

							int cropX = 0, cropY = 0, cropW = clientW, cropH = clientH;
							if (srcMode == 2)
							{
								// Player 1: Left half of split-screen
								cropX = 0;
								cropY = 0;
								cropW = std::max(32, clientW / 2);
								cropH = clientH;
							}
							else if (srcMode == 3)
							{
								// Player 2: Right half of split-screen
								cropX = clientW / 2;
								cropY = 0;
								cropW = std::max(32, clientW - cropX);
								cropH = clientH;
							}

							// Limit output resolution to 1920 max edge for high stream performance
							int outW = cropW;
							int outH = cropH;
							if (outW > 1920)
							{
								outH = (outH * 1920) / outW;
								outW = 1920;
							}

							HDC hdcScaled = CreateCompatibleDC(hdcWin);
							HBITMAP hbmScaled = CreateCompatibleBitmap(hdcWin, outW, outH);
							HGDIOBJ oldScaled = SelectObject(hdcScaled, hbmScaled);

							SetStretchBltMode(hdcScaled, HALFTONE);
							StretchBlt(hdcScaled, 0, 0, outW, outH, hdcFull, cropX, cropY, cropW, cropH, SRCCOPY);

							{
								auto* bmp = new Gdiplus::Bitmap(hbmScaled, nullptr);
								if (bmp && bmp->GetLastStatus() == Gdiplus::Ok)
								{
									IStream* stream = nullptr;
									if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) == S_OK)
									{
										Gdiplus::EncoderParameters encoderParameters{};
										encoderParameters.Count = 1;
										encoderParameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
										encoderParameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
										encoderParameters.Parameter[0].NumberOfValues = 1;
										ULONG quality = std::clamp(jpegQuality.load(), 40, 100);
										encoderParameters.Parameter[0].Value = &quality;

										bmp->Save(stream, &clsidJpeg, &encoderParameters);

										STATSTG stat{};
										if (stream->Stat(&stat, STATFLAG_NONAME) == S_OK)
										{
											const size_t sz = static_cast<size_t>(stat.cbSize.QuadPart);
											if (sz > 0)
											{
												std::vector<uint8_t> compressed(sz);
												LARGE_INTEGER zero{};
												stream->Seek(zero, STREAM_SEEK_SET, nullptr);
												ULONG bytesRead = 0;
												stream->Read(compressed.data(), static_cast<ULONG>(sz), &bytesRead);

												{
													std::lock_guard<std::mutex> lk(frameMutex);
													latestJpeg = std::move(compressed);
													++latestSeq;
												}
												++totalFramesCaptured;
												lastCaptureW.store(outW);
												lastCaptureH.store(outH);
											}
										}
										stream->Release();
									}
								}
								delete bmp;
							}

							SelectObject(hdcScaled, oldScaled);
							DeleteObject(hbmScaled);
							DeleteDC(hdcScaled);

							SelectObject(hdcFull, oldFull);
							DeleteObject(hbmFull);
							DeleteDC(hdcFull);

							ReleaseDC(hwndTarget, hdcWin);
						}
					}
				}

				const int fps = std::clamp(targetFps.load(), 15, 90);
				const auto targetDur = std::chrono::milliseconds(1000 / fps);
				const auto elapsed = std::chrono::steady_clock::now() - frameStart;
				if (elapsed < targetDur)
					std::this_thread::sleep_for(targetDur - elapsed);
				else
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}

			Gdiplus::GdiplusShutdown(gdiplusToken);
		}

		std::string BuildQuestWebXRAppHtml()
		{
			const int srcMode = targetSource.load();
			const int defMode = defaultXrMode.load();
			const std::string playerBadge = (srcMode == 1 || srcMode == 3) ? "PLAYER 2 (Quest Headset)" : "PLAYER 1 (Quest Headset)";
			const std::string modeHint = (defMode == 0) ? "immersive-ar" : "immersive-vr";
			const int targetPlayerIdx = (srcMode == 1 || srcMode == 3) ? 1 : 0;

			std::ostringstream h;
			h << R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"/>
<title>CemuX Quest VR & Mixed Reality Broadcast</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;user-select:none}
body{background:#090b10;color:#eef2ff;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;overflow:hidden;height:100vh;display:flex;flex-direction:column}
#hud{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;padding:10px 18px;background:rgba(15,20,32,0.94);border-bottom:1px solid #26324d;z-index:10}
.brand{display:flex;align-items:center;gap:12px}
.logo{width:34px;height:34px;border-radius:9px;background:linear-gradient(135deg,#00d2ff,#3a7bd5);display:flex;align-items:center;justify-content:center;font-weight:900;font-size:18px;color:#fff;box-shadow:0 0 14px rgba(0,210,255,0.45)}
.title{font-weight:800;font-size:17px;letter-spacing:0.4px}
.badge{padding:4px 10px;border-radius:999px;font-size:12px;font-weight:700;background:#1d2842;color:#67e8f9;border:1px solid #2dd4bf}
.ctrl-badge{padding:4px 10px;border-radius:999px;font-size:12px;font-weight:700;background:#1e293b;color:#94a3b8;border:1px solid #475569;transition:all 0.2s}
.ctrl-badge.ok{background:#064e3b;color:#6ee7b7;border-color:#10b981}
.actions{display:flex;gap:10px;flex-wrap:wrap}
button{cursor:pointer;border:none;border-radius:10px;padding:10px 16px;font-weight:700;font-size:14px;color:#fff;transition:transform 0.12s,filter 0.12s}
button:active{transform:scale(0.96)}
.btn-mr{background:linear-gradient(135deg,#10b981,#059669);box-shadow:0 0 16px rgba(16,185,129,0.4)}
.btn-vr{background:linear-gradient(135deg,#6366f1,#4f46e5);box-shadow:0 0 16px rgba(99,102,241,0.4)}
.btn-fs{background:#334155}
#stage{flex:1;position:relative;display:flex;align-items:center;justify-content:center;background:#000}
#screen2d{max-width:100%;max-height:100%;width:100%;height:100%;object-fit:contain}
#xrCanvas{display:none;width:100%;height:100%}
#overlayTip{position:absolute;bottom:14px;left:50%;transform:translateX(-50%);background:rgba(15,23,42,0.88);border:1px solid #334155;padding:8px 18px;border-radius:999px;font-size:13px;color:#cbd5e1;pointer-events:none;text-align:center;box-shadow:0 4px 16px rgba(0,0,0,0.6)}
#webxrNotice{display:none;position:absolute;top:65px;left:50%;transform:translateX(-50%);max-width:680px;width:90%;background:rgba(69,26,3,0.94);border:1px solid #f59e0b;color:#fef3c7;padding:12px 18px;border-radius:12px;font-size:13px;line-height:1.5;box-shadow:0 8px 30px rgba(0,0,0,0.6);z-index:50}
</style>
</head>
<body>
<div id="hud">
  <div class="brand">
    <div class="logo">X</div>
    <div>
      <div class="title">CemuX Quest VR / MR Studio</div>
      <div style="font-size:11px;color:#94a3b8" id="statsText">Connecting to CemuX stream...</div>
    </div>
    <span class="badge">)HTML" << playerBadge << R"HTML(</span>
    <span class="ctrl-badge" id="padBadge">🎮 Controller Listening...</span>
  </div>
  <div class="actions">
    <button class="btn-mr" id="btnMR">🌐 Mixed Reality (Passthrough)</button>
    <button class="btn-vr" id="btnVR">🥽 Full-Screen Immersive VR</button>
    <button class="btn-fs" id="btnFS">🖥️ 2D Fullscreen Cinema</button>
  </div>
</div>
<div id="stage">
  <img id="screen2d" alt="CemuX Live Broadcast"/>
  <canvas id="xrCanvas"></canvas>
  <div id="webxrNotice">
    <div style="font-weight:800;font-size:14px;color:#fbbf24;margin-bottom:4px">⚠️ Meta Quest Browser WebXR Secure Context Notice</div>
    Meta Quest Browser requires a <strong>Secure Context</strong> for WebXR. If entering Mixed Reality or VR fails:<br/>
    • <strong>1-Click Fix via USB / Wireless ADB:</strong> In CemuX click <strong>'⚡ 1-Click Fix WebXR via ADB'</strong> to open <code>http://localhost:8765</code> (works instantly!).<br/>
    • <strong>Wi-Fi Setup:</strong> Open <code>chrome://flags/#unsafely-treat-insecure-origin-as-secure</code>, enable it, add this URL, and click Relaunch.<br/>
    • <strong>Play Now:</strong> Click <strong>🖥️ 2D Fullscreen Cinema</strong> to play in the headset browser immediately with full controller support!
    <div style="text-align:right;margin-top:6px"><button onclick="document.getElementById('webxrNotice').style.display='none'" style="padding:4px 12px;background:#b45309;font-size:12px">Dismiss</button></div>
  </div>
  <div id="overlayTip">🎮 Controls: Sticks = Move & Look • Triggers = ZL/ZR • Grips = L/R • A/B/X/Y = Action Buttons • Hold Both Grips + Stick = Adjust Screen • A+B = Recenter</div>
</div>
<script>
const img2d = document.getElementById('screen2d');
const statsText = document.getElementById('statsText');
const padBadge = document.getElementById('padBadge');
const xrCanvas = document.getElementById('xrCanvas');
const defaultMode = ")HTML" << modeHint << R"HTML(";
const playerIndex = )HTML" << targetPlayerIdx << R"HTML(;

let frameCount = 0;
let lastFpsTime = performance.now();
let currentFps = 0;
let loadingFrame = false;
let xrSession = null;
let gl = null;
let prog = null;
let tex = null;
let vbo = null;
let refSpace = null;

// Screen placement parameters in meters
let screenWidth = 3.2;
let screenDist = 2.4;
let screenHeightOffset = 0.05;
let yawOffset = 0.0;
let needRecenter = true;

// Controller and Gamepad Input State
let curButtons = 0;
let curLX = 0.0, curLY = 0.0, curRX = 0.0, curRY = 0.0;
let lastSentButtons = 0;
let lastSentLX = 0.0, lastSentLY = 0.0, lastSentRX = 0.0, lastSentRY = 0.0;
let lastInputSentTime = 0;

// Wii U VPAD bit definitions
const VPAD_A = 0x8000;
const VPAD_B = 0x4000;
const VPAD_X = 0x2000;
const VPAD_Y = 0x1000;
const VPAD_L = 0x0020;
const VPAD_R = 0x0010;
const VPAD_ZL = 0x0080;
const VPAD_ZR = 0x0040;
const VPAD_PLUS = 0x0008;
const VPAD_MINUS = 0x0004;
const VPAD_HOME = 0x0002;
const VPAD_UP = 0x0200;
const VPAD_DOWN = 0x0100;
const VPAD_LEFT = 0x0800;
const VPAD_RIGHT = 0x0400;
const VPAD_STICK_L = 0x00040000;
const VPAD_STICK_R = 0x00020000;

function pollControllers() {
  let btns = 0;
  let lx = 0.0, ly = 0.0, rx = 0.0, ry = 0.0;
  let hasActivePad = false;
  let padDesc = "";

  // 1. Standard HTML5 Gamepad API (Bluetooth gamepads paired to Quest or PC)
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (let i = 0; i < pads.length; i++) {
    const p = pads[i];
    if (!p || !p.connected) continue;
    hasActivePad = true;
    padDesc = p.id || "Bluetooth Gamepad";

    if (p.buttons[0]?.pressed) btns |= VPAD_B;
    if (p.buttons[1]?.pressed) btns |= VPAD_A;
    if (p.buttons[2]?.pressed) btns |= VPAD_Y;
    if (p.buttons[3]?.pressed) btns |= VPAD_X;
    if (p.buttons[4]?.pressed) btns |= VPAD_L;
    if (p.buttons[5]?.pressed) btns |= VPAD_R;
    if (p.buttons[6]?.pressed || (p.buttons[6]?.value > 0.2)) btns |= VPAD_ZL;
    if (p.buttons[7]?.pressed || (p.buttons[7]?.value > 0.2)) btns |= VPAD_ZR;
    if (p.buttons[8]?.pressed) btns |= VPAD_MINUS;
    if (p.buttons[9]?.pressed) btns |= VPAD_PLUS;
    if (p.buttons[10]?.pressed) btns |= VPAD_STICK_L;
    if (p.buttons[11]?.pressed) btns |= VPAD_STICK_R;
    if (p.buttons[12]?.pressed) btns |= VPAD_UP;
    if (p.buttons[13]?.pressed) btns |= VPAD_DOWN;
    if (p.buttons[14]?.pressed) btns |= VPAD_LEFT;
    if (p.buttons[15]?.pressed) btns |= VPAD_RIGHT;
    if (p.buttons[16]?.pressed) btns |= VPAD_HOME;

    if (p.axes.length >= 2) {
      if (Math.abs(p.axes[0]) > 0.12) lx = p.axes[0];
      if (Math.abs(p.axes[1]) > 0.12) ly = -p.axes[1]; // Invert so Up is positive
    }
    if (p.axes.length >= 4) {
      if (Math.abs(p.axes[2]) > 0.12) rx = p.axes[2];
      if (Math.abs(p.axes[3]) > 0.12) ry = -p.axes[3]; // Invert so Up is positive
    }
    break;
  }

  // 2. Meta Quest Touch Controllers in WebXR
  if (xrSession && xrSession.inputSources) {
    let leftGrip = false, rightGrip = false;
    for (const src of xrSession.inputSources) {
      const g = src.gamepad;
      if (!g) continue;
      if (src.handedness === 'left' && (g.buttons[1]?.pressed || g.buttons[1]?.value > 0.5)) leftGrip = true;
      if (src.handedness === 'right' && (g.buttons[1]?.pressed || g.buttons[1]?.value > 0.5)) rightGrip = true;
    }
    const isAdjusting = leftGrip && rightGrip;

    for (const src of xrSession.inputSources) {
      const g = src.gamepad;
      if (!g) continue;
      hasActivePad = true;
      if (!padDesc) padDesc = "Quest Touch Controllers";

      if (src.handedness === 'left') {
        if (g.buttons[0]?.pressed || g.buttons[0]?.value > 0.2) btns |= VPAD_ZL;
        if (!isAdjusting && (g.buttons[1]?.pressed || g.buttons[1]?.value > 0.2)) btns |= VPAD_L;
        if (g.buttons[3]?.pressed) btns |= VPAD_STICK_L;
        if (g.buttons[4]?.pressed) btns |= VPAD_X;
        if (g.buttons[5]?.pressed) btns |= VPAD_Y;

        const ax = (g.axes.length >= 4) ? g.axes[2] : g.axes[0];
        const ay = (g.axes.length >= 4) ? g.axes[3] : g.axes[1];
        if (isAdjusting) {
          if (Math.abs(ay) > 0.2) screenWidth = Math.max(1.2, Math.min(6.5, screenWidth - ay * 0.03));
          if (Math.abs(ax) > 0.2) screenDist  = Math.max(1.2, Math.min(5.5, screenDist  + ax * 0.03));
        } else {
          if (Math.abs(ax) > 0.12) lx = ax;
          if (Math.abs(ay) > 0.12) ly = -ay;
        }
      } else if (src.handedness === 'right') {
        if (g.buttons[0]?.pressed || g.buttons[0]?.value > 0.2) btns |= VPAD_ZR;
        if (!isAdjusting && (g.buttons[1]?.pressed || g.buttons[1]?.value > 0.2)) btns |= VPAD_R;
        if (g.buttons[3]?.pressed) btns |= VPAD_STICK_R;
        if (g.buttons[4]?.pressed) btns |= VPAD_A;
        if (g.buttons[5]?.pressed) btns |= VPAD_B;

        // A + B pressed simultaneously on right controller recenters
        if (g.buttons[4]?.pressed && g.buttons[5]?.pressed) {
          needRecenter = true;
        }

        const ax = (g.axes.length >= 4) ? g.axes[2] : g.axes[0];
        const ay = (g.axes.length >= 4) ? g.axes[3] : g.axes[1];
        if (isAdjusting) {
          if (Math.abs(ax) > 0.2) screenDist = Math.max(1.2, Math.min(5.5, screenDist + ax * 0.03));
        } else {
          if (Math.abs(ax) > 0.12) rx = ax;
          if (Math.abs(ay) > 0.12) ry = -ay;
        }
      }
    }
  }

  curButtons = btns;
  curLX = lx; curLY = ly;
  curRX = rx; curRY = ry;

  // Update UI badge
  if (hasActivePad) {
    padBadge.classList.add('ok');
    const actionText = (btns !== 0 || Math.abs(lx) > 0.2 || Math.abs(ly) > 0.2) ? " (Sending Input)" : " (Connected)";
    padBadge.textContent = "🎮 " + (padDesc.length > 22 ? padDesc.substr(0, 20) + ".." : padDesc) + actionText;
  } else {
    padBadge.classList.remove('ok');
    padBadge.textContent = "🎮 Sync Wireless Controller (Quest/PC)";
  }

  // Fast transmission if changed or periodically every 50ms
  const now = performance.now();
  const changed = (curButtons !== lastSentButtons) ||
                  (Math.abs(curLX - lastSentLX) > 0.04) ||
                  (Math.abs(curLY - lastSentLY) > 0.04) ||
                  (Math.abs(curRX - lastSentRX) > 0.04) ||
                  (Math.abs(curRY - lastSentRY) > 0.04);
  if (changed || (now - lastInputSentTime >= 50 && hasActivePad)) {
    lastSentButtons = curButtons;
    lastSentLX = curLX; lastSentLY = curLY;
    lastSentRX = curRX; lastSentRY = curRY;
    lastInputSentTime = now;
    const url = `/input?p=${playerIndex}&btn=${curButtons}&lx=${curLX.toFixed(3)}&ly=${curLY.toFixed(3)}&rx=${curRX.toFixed(3)}&ry=${curRY.toFixed(3)}`;
    fetch(url, { method: 'GET', keepalive: true, cache: 'no-store' }).catch(() => {});
  }
}
setInterval(pollControllers, 16);

function pollFrame() {
  if (loadingFrame) return;
  loadingFrame = true;
  const nextImg = new Image();
  nextImg.crossOrigin = "anonymous";
  nextImg.onload = () => {
    img2d.src = nextImg.src;
    if (gl && tex && xrSession) {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, nextImg);
    }
    frameCount++;
    const now = performance.now();
    if (now - lastFpsTime >= 1000) {
      currentFps = Math.round((frameCount * 1000) / (now - lastFpsTime));
      frameCount = 0;
      lastFpsTime = now;
      statsText.textContent = `Live Stream: ${nextImg.naturalWidth}x${nextImg.naturalHeight} @ ${currentFps} FPS`;
    }
    loadingFrame = false;
    requestAnimationFrame(pollFrame);
  };
  nextImg.onerror = () => {
    loadingFrame = false;
    setTimeout(pollFrame, 120);
  };
  nextImg.src = `/frame.jpg?t=${ performance.now() }&p=${playerIndex}&btn=${curButtons}&lx=${curLX.toFixed(3)}&ly=${curLY.toFixed(3)}&rx=${curRX.toFixed(3)}&ry=${curRY.toFixed(3)}`;
}
pollFrame();

document.getElementById('btnFS').onclick = () => {
  const el = document.getElementById('stage');
  if (!document.fullscreenElement) el.requestFullscreen();
  else document.exitFullscreen();
};

function initWebGL() {
  if (gl) return true;
  gl = xrCanvas.getContext('webgl2', { xrCompatible: true, alpha: true }) ||
       xrCanvas.getContext('webgl', { xrCompatible: true, alpha: true });
  if (!gl) return false;

  const vsSrc = `
    attribute vec3 aPos;
    attribute vec2 aUv;
    uniform mat4 uProj;
    uniform mat4 uView;
    uniform float uWidth;
    uniform float uAspect;
    uniform float uDist;
    uniform float uHeight;
    uniform float uYaw;
    varying vec2 vUv;
    void main() {
      float h = uWidth / uAspect;
      // Slight cinema curve
      float x = aPos.x * (uWidth * 0.5);
      float y = aPos.y * (h * 0.5) + uHeight;
      float curve = (x * x) * 0.08;
      float z = -uDist + curve;
      float c = cos(uYaw), s = sin(uYaw);
      vec3 worldPos = vec3(x * c + z * s, y, -x * s + z * c);
      gl_Position = uProj * uView * vec4(worldPos, 1.0);
      vUv = aUv;
    }
  `;
  const fsSrc = `
    precision mediump float;
    varying vec2 vUv;
    uniform sampler2D uTex;
    void main() {
      gl_FragColor = texture2D(uTex, vUv);
    }
  `;
  function compile(type, src) {
    const sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    return sh;
  }
  prog = gl.createProgram();
  gl.attachShader(prog, compile(gl.VERTEX_SHADER, vsSrc));
  gl.attachShader(prog, compile(gl.FRAGMENT_SHADER, fsSrc));
  gl.linkProgram(prog);

  // Subdivided curved screen mesh (16 segments horizontally)
  const verts = [];
  const segs = 16;
  for (let i = 0; i < segs; i++) {
    const u0 = i / segs, u1 = (i + 1) / segs;
    const x0 = u0 * 2.0 - 1.0, x1 = u1 * 2.0 - 1.0;
    verts.push(
      x0, -1, 0, u0, 1,
      x1, -1, 0, u1, 1,
      x0,  1, 0, u0, 0,
      x0,  1, 0, u0, 0,
      x1, -1, 0, u1, 1,
      x1,  1, 0, u1, 0
    );
  }
  vbo = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(verts), gl.STATIC_DRAW);

  tex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, tex);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  return true;
}

async function startXR(mode) {
  if (!navigator.xr) {
    document.getElementById('webxrNotice').style.display = 'block';
    alert("WebXR is not available in this context (insecure origin or not inside Meta Quest Browser).\n\nPlease check the instructions banner at the top, or click '2D Fullscreen Cinema' below to play right now!");
    return;
  }
  if (xrSession) {
    await xrSession.end();
    return;
  }
  if (!initWebGL()) return;

  try {
    const supported = await navigator.xr.isSessionSupported(mode);
    if (!supported) {
      mode = (mode === 'immersive-ar') ? 'immersive-vr' : 'immersive-ar';
    }
    xrSession = await navigator.xr.requestSession(mode, {
      optionalFeatures: ['local-floor', 'bounded-floor', 'hand-tracking']
    });
    xrSession.addEventListener('end', () => { xrSession = null; });
    await gl.makeXRCompatible();
    xrSession.updateRenderState({ baseLayer: new XRWebGLLayer(xrSession, gl) });
    refSpace = await xrSession.requestReferenceSpace('local-floor').catch(() => xrSession.requestReferenceSpace('local'));
    needRecenter = true;
    const isAR = (mode === 'immersive-ar');
    xrSession.requestAnimationFrame((t, frame) => onXRFrame(t, frame, isAR));
  } catch (e) {
    document.getElementById('webxrNotice').style.display = 'block';
    alert("Could not start WebXR session: " + e.message + "\n\nTip: You can use 1-Click ADB Fix in CemuX to forward localhost, or play in 2D Fullscreen Cinema mode!");
  }
}

function onXRFrame(time, frame, isAR) {
  const session = frame.session;
  session.requestAnimationFrame((t, f) => onXRFrame(t, f, isAR));
  const pose = frame.getViewerPose(refSpace);
  if (!pose) return;

  pollControllers();

  if (needRecenter) {
    const q = pose.transform.orientation;
    const siny_cosp = 2 * (q.w * q.y + q.z * q.x);
    const cosy_cosp = 1 - 2 * (q.x * q.x + q.y * q.y);
    yawOffset = Math.atan2(siny_cosp, cosy_cosp);
    screenHeightOffset = pose.transform.position.y;
    needRecenter = false;
  }

  const layer = session.renderState.baseLayer;
  gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
  if (isAR) {
    // Transparent background enables Quest Mixed Reality color passthrough!
    gl.clearColor(0.0, 0.0, 0.0, 0.0);
  } else {
    // Dark cinema void for Full-Screen Immersive VR
    gl.clearColor(0.02, 0.025, 0.04, 1.0);
  }
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

  gl.useProgram(prog);
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  const aPos = gl.getAttribLocation(prog, 'aPos');
  const aUv  = gl.getAttribLocation(prog, 'aUv');
  gl.enableVertexAttribArray(aPos);
  gl.enableVertexAttribArray(aUv);
  gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 20, 0);
  gl.vertexAttribPointer(aUv,  2, gl.FLOAT, false, 20, 12);

  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, tex);
  gl.uniform1i(gl.getUniformLocation(prog, 'uTex'), 0);

  const aspect = (img2d.naturalWidth > 0 && img2d.naturalHeight > 0) ? (img2d.naturalWidth / img2d.naturalHeight) : (16.0 / 9.0);
  gl.uniform1f(gl.getUniformLocation(prog, 'uWidth'), screenWidth);
  gl.uniform1f(gl.getUniformLocation(prog, 'uAspect'), aspect);
  gl.uniform1f(gl.getUniformLocation(prog, 'uDist'), screenDist);
  gl.uniform1f(gl.getUniformLocation(prog, 'uHeight'), screenHeightOffset);
  gl.uniform1f(gl.getUniformLocation(prog, 'uYaw'), yawOffset);

  for (const view of pose.views) {
    const vp = layer.getViewport(view);
    gl.viewport(vp.x, vp.y, vp.width, vp.height);
    gl.uniformMatrix4fv(gl.getUniformLocation(prog, 'uProj'), false, view.projectionMatrix);
    gl.uniformMatrix4fv(gl.getUniformLocation(prog, 'uView'), false, view.transform.inverse.matrix);
    gl.drawArrays(gl.TRIANGLES, 0, 16 * 6);
  }
}

document.getElementById('btnMR').onclick = () => startXR('immersive-ar');
document.getElementById('btnVR').onclick = () => startXR('immersive-vr');
</script>
</body>
</html>)HTML";
			return h.str();
		}

		void HandleClient(SOCKET clientSock)
		{
			DWORD timeoutMs = 4000;
			setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
			setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

			char reqBuf[2048]{};
			int recvd = recv(clientSock, reqBuf, sizeof(reqBuf) - 1, 0);
			if (recvd <= 0)
			{
				closesocket(clientSock);
				return;
			}
			std::string req(reqBuf, recvd);

			if (req.rfind("OPTIONS ", 0) == 0)
			{
				const char* preflight =
					"HTTP/1.1 204 No Content\r\n"
					"Access-Control-Allow-Origin: *\r\n"
					"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
					"Access-Control-Allow-Headers: *\r\n"
					"Connection: close\r\n\r\n";
				send(clientSock, preflight, static_cast<int>(strlen(preflight)), 0);
			}
			else if (req.rfind("GET /input", 0) == 0)
			{
				ParseAndApplyQuestInput(req);
				const char* resp =
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/json\r\n"
					"Content-Length: 11\r\n"
					"Access-Control-Allow-Origin: *\r\n"
					"Cache-Control: no-store, no-cache\r\n"
					"Connection: close\r\n\r\n"
					"{\"ok\":true}";
				send(clientSock, resp, static_cast<int>(strlen(resp)), 0);
			}
			else if (req.rfind("GET /frame.jpg", 0) == 0)
			{
				ParseAndApplyQuestInput(req);
				std::vector<uint8_t> jpegCopy;
				uint64_t seq = 0;
				{
					std::lock_guard<std::mutex> lk(frameMutex);
					jpegCopy = latestJpeg;
					seq = latestSeq;
				}
				if (jpegCopy.empty())
				{
					const char* notReady = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
					send(clientSock, notReady, static_cast<int>(strlen(notReady)), 0);
				}
				else
				{
					std::string hdr = fmt::format(
						"HTTP/1.1 200 OK\r\n"
						"Content-Type: image/jpeg\r\n"
						"Content-Length: {}\r\n"
						"Cache-Control: no-store, no-cache, must-revalidate\r\n"
						"Access-Control-Allow-Origin: *\r\n"
						"X-Frame-Seq: {}\r\n"
						"Connection: close\r\n\r\n",
						jpegCopy.size(), seq);
					send(clientSock, hdr.c_str(), static_cast<int>(hdr.size()), 0);
					send(clientSock, reinterpret_cast<const char*>(jpegCopy.data()), static_cast<int>(jpegCopy.size()), 0);
					++totalFramesServed;
				}
			}
			else if (req.rfind("GET /status", 0) == 0)
			{
				std::string body = fmt::format(
					"{{\"running\":true,\"targetSource\":{},\"defaultXrMode\":{},\"fps\":{},\"framesCaptured\":{},\"framesServed\":{}}}",
					targetSource.load(), defaultXrMode.load(), targetFps.load(),
					totalFramesCaptured.load(), totalFramesServed.load());
				std::string hdr = fmt::format(
					"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
					body.size());
				send(clientSock, hdr.c_str(), static_cast<int>(hdr.size()), 0);
				send(clientSock, body.c_str(), static_cast<int>(body.size()), 0);
			}
			else
			{
				std::string html = BuildQuestWebXRAppHtml();
				std::string hdr = fmt::format(
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: text/html; charset=utf-8\r\n"
					"Content-Length: {}\r\n"
					"Cache-Control: no-store\r\n"
					"Connection: close\r\n\r\n",
					html.size());
				send(clientSock, hdr.c_str(), static_cast<int>(hdr.size()), 0);
				send(clientSock, html.c_str(), static_cast<int>(html.size()), 0);
			}

			closesocket(clientSock);
		}

		void ServerLoop()
		{
			WSADATA wsa{};
			WSAStartup(MAKEWORD(2, 2), &wsa);

			SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (srv == INVALID_SOCKET)
			{
				running.store(false);
				return;
			}

			BOOL reuse = TRUE;
			setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

			uint16_t chosenPort = port.load();
			bool bound = false;
			for (int attempt = 0; attempt < 5; ++attempt)
			{
				sockaddr_in addr{};
				addr.sin_family = AF_INET;
				addr.sin_addr.s_addr = INADDR_ANY;
				addr.sin_port = htons(chosenPort);
				if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
				{
					port.store(chosenPort);
					bound = true;
					break;
				}
				++chosenPort;
			}

			if (!bound || listen(srv, SOMAXCONN) != 0)
			{
				closesocket(srv);
				running.store(false);
				return;
			}

			listenSock.store( static_cast<uintptr_t>(srv) );

			while (running.load())
			{
				fd_set readSet;
				FD_ZERO(&readSet);
				FD_SET(srv, &readSet);
				timeval tv{0, 200000}; // 200ms poll so Stop() wakes up fast
				int sel = select(0, &readSet, nullptr, nullptr, &tv);
				if (sel > 0 && FD_ISSET(srv, &readSet))
				{
					sockaddr_in clientAddr{};
					int addrLen = sizeof(clientAddr);
					SOCKET clientSock = accept(srv, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
					if (clientSock != INVALID_SOCKET)
					{
						std::thread(&QuestBroadcastService::HandleClient, this, clientSock).detach();
					}
				}
			}

			SOCKET oldSrv = static_cast<SOCKET>(listenSock.exchange(static_cast<uintptr_t>(INVALID_SOCKET)));
			if (oldSrv != INVALID_SOCKET)
				closesocket(oldSrv);
		}
#endif

		void Start()
		{
			if (running.exchange(true))
				return;
			totalFramesCaptured.store(0);
			totalFramesServed.store(0);
#if BOOST_OS_WINDOWS
			captureThread = std::thread(&QuestBroadcastService::CaptureLoop, this);
			serverThread  = std::thread(&QuestBroadcastService::ServerLoop, this);
#endif
		}

		void Stop()
		{
			if (!running.exchange(false))
				return;
#if BOOST_OS_WINDOWS
			SOCKET s = static_cast<SOCKET>(listenSock.exchange(static_cast<uintptr_t>(INVALID_SOCKET)));
			if (s != INVALID_SOCKET)
				closesocket(s);
#endif
			if (captureThread.joinable())
				captureThread.join();
			if (serverThread.joinable())
				serverThread.join();
		}
	};
} // namespace

void QuestVRWindow::SetMainAndPadHWNDs(uintptr_t mainHwnd, uintptr_t padHwnd)
{
	auto& svc = QuestBroadcastService::Instance();
	if (mainHwnd != 0)
		svc.mainHwnd.store(mainHwnd);
	svc.padHwnd.store(padHwnd);
}

bool QuestVRWindow::IsBroadcastRunning()
{
	return QuestBroadcastService::Instance().running.load();
}

void QuestVRWindow::ShutdownBroadcast()
{
	QuestBroadcastService::Instance().Stop();
}

void QuestVRWindow::SetQuestInput(int player, uint32_t buttons, float lx, float ly, float rx, float ry)
{
	auto& svc = QuestBroadcastService::Instance();
	if (player < 0 || player >= 2)
		return;
	uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	svc.questInput[player].buttons.store(buttons);
	svc.questInput[player].stickLX.store(std::clamp(lx, -1.0f, 1.0f));
	svc.questInput[player].stickLY.store(std::clamp(ly, -1.0f, 1.0f));
	svc.questInput[player].stickRX.store(std::clamp(rx, -1.0f, 1.0f));
	svc.questInput[player].stickRY.store(std::clamp(ry, -1.0f, 1.0f));
	svc.questInput[player].lastTimestampMs.store(nowMs);
}

bool QuestVRWindow::GetQuestInput(int player, uint32_t& buttons, float& lx, float& ly, float& rx, float& ry)
{
	auto& svc = QuestBroadcastService::Instance();
	if (player < 0 || player >= 2)
		return false;
	uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	uint64_t last = svc.questInput[player].lastTimestampMs.load();
	if (last == 0 || (nowMs - last) > 1000)
		return false;

	buttons = svc.questInput[player].buttons.load();
	lx = svc.questInput[player].stickLX.load();
	ly = svc.questInput[player].stickLY.load();
	rx = svc.questInput[player].stickRX.load();
	ry = svc.questInput[player].stickRY.load();
	return true;
}

bool QuestVRWindow::HasRecentQuestInput(int player)
{
	auto& svc = QuestBroadcastService::Instance();
	if (player < 0 || player >= 2)
		return false;
	uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	uint64_t last = svc.questInput[player].lastTimestampMs.load();
	return (last > 0 && (nowMs - last) <= 1000);
}

uint32_t QuestVRWindow::ConvertVpadToWpadButtons(uint32_t vpad)
{
	uint32_t w = 0;
	if (vpad & 0x0200) w |= kProButton_Up;
	if (vpad & 0x0100) w |= kProButton_Down;
	if (vpad & 0x0800) w |= kProButton_Left;
	if (vpad & 0x0400) w |= kProButton_Right;
	if (vpad & 0x8000) w |= kProButton_A;
	if (vpad & 0x4000) w |= kProButton_B;
	if (vpad & 0x2000) w |= kProButton_X;
	if (vpad & 0x1000) w |= kProButton_Y;
	if (vpad & 0x0020) w |= kProButton_L;
	if (vpad & 0x0010) w |= kProButton_R;
	if (vpad & 0x0080) w |= kProButton_ZL;
	if (vpad & 0x0040) w |= kProButton_ZR;
	if (vpad & 0x0008) w |= kProButton_Plus;
	if (vpad & 0x0004) w |= kProButton_Minus;
	if (vpad & 0x0002) w |= kProButton_Home;
	if (vpad & 0x00040000) w |= kProButton_StickL;
	if (vpad & 0x00020000) w |= kProButton_StickR;
	return w;
}

std::string QuestVRWindow::FindAdbExecutable()
{
	static const char* s_knownPaths[] = {
		"C:\\adb\\platform-tools\\adb.exe",
		"C:\\platform-tools\\adb.exe",
		"C:\\adb\\adb.exe",
	};
	for (const char* p : s_knownPaths)
	{
		std::error_code ec;
		if (std::filesystem::exists(p, ec))
			return p;
	}

#if BOOST_OS_WINDOWS
	char localApp[MAX_PATH]{};
	if (GetEnvironmentVariableA("LOCALAPPDATA", localApp, MAX_PATH) > 0)
	{
		std::string p = std::string(localApp) + "\\Android\\Sdk\\platform-tools\\adb.exe";
		std::error_code ec;
		if (std::filesystem::exists(p, ec))
			return p;
	}

	char appData[MAX_PATH]{};
	if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH) > 0)
	{
		std::string p = std::string(appData) + "\\SideQuest\\platform-tools\\adb.exe";
		std::error_code ec;
		if (std::filesystem::exists(p, ec))
			return p;
	}

	char found[MAX_PATH]{};
	if (SearchPathA(nullptr, "adb.exe", nullptr, MAX_PATH, found, nullptr) > 0)
	{
		return found;
	}
#endif

	return {};
}

bool QuestVRWindow::RunAdbCommand(const std::string& args, std::string* output)
{
#if BOOST_OS_WINDOWS
	std::string adbPath = FindAdbExecutable();
	if (adbPath.empty())
	{
		if (output)
			*output = "adb.exe not found on system";
		return false;
	}

	std::string cmd = "\"" + adbPath + "\" " + args;

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE hReadPipe = NULL;
	HANDLE hWritePipe = NULL;
	if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
		return false;
	SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si{};
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESTDHANDLES;
	si.hStdOutput = hWritePipe;
	si.hStdError = hWritePipe;

	PROCESS_INFORMATION pi{};
	std::vector<char> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back('\0');

	BOOL ok = CreateProcessA(
		nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

	CloseHandle(hWritePipe);

	if (!ok)
	{
		CloseHandle(hReadPipe);
		if (output)
			*output = "Failed to launch adb process";
		return false;
	}

	std::string outStr;
	char buffer[512];
	DWORD bytesRead = 0;
	while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0)
	{
		buffer[bytesRead] = '\0';
		outStr += buffer;
	}
	CloseHandle(hReadPipe);

	WaitForSingleObject(pi.hProcess, 5000);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	if (output)
		*output = outStr;
	return (exitCode == 0);
#else
	return false;
#endif
}

QuestVRWindow::QuestVRWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Quest VR & Mixed Reality Broadcast Studio (Player 1 / Player 2)"),
			   wxDefaultPosition, wxSize(740, 620), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	auto& svc = QuestBroadcastService::Instance();

	auto* rootSizer = new wxBoxSizer(wxVERTICAL);

	// Header banner
	auto* headerBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Meta Quest 2 / Quest 3 / Quest Pro Wireless Broadcast"));
	auto* introText = new wxStaticText(
		headerBox->GetStaticBox(), wxID_ANY,
		_("Stream CemuX directly to a Meta Quest headset over local Wi-Fi in either Mixed Reality Passthrough "
		  "(floating screen in your room) or Full-Screen Immersive VR Cinema - for either Player 1 or Player 2!"));
	introText->Wrap(680);
	headerBox->Add(introText, 0, wxALL | wxEXPAND, 6);
	rootSizer->Add(headerBox, 0, wxALL | wxEXPAND, 8);

	// 1. Wireless Controller Readiness Section
	auto* ctrlBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Step 1: Sync Wireless Controller First (Recommended)"));
	m_controllerStatusText = new wxStaticText(ctrlBox->GetStaticBox(), wxID_ANY, wxEmptyString);
	ctrlBox->Add(m_controllerStatusText, 0, wxALL | wxEXPAND, 6);

	auto* ctrlBtnRow = new wxBoxSizer(wxHORIZONTAL);
	auto* inputBtn = new wxButton(ctrlBox->GetStaticBox(), wxID_ANY, _("Configure Player 1 / Player 2 Controllers..."));
	inputBtn->Bind(wxEVT_BUTTON, &QuestVRWindow::OnOpenInputSettings, this);
	ctrlBtnRow->Add(inputBtn, 0, wxALL, 4);
	ctrlBox->Add(ctrlBtnRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
	rootSizer->Add(ctrlBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

	// 2. Player & Viewing Mode Settings
	auto* cfgBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Step 2: Choose Target Player && Quest VR / Mixed Reality Mode"));
	auto* grid = new wxFlexGridSizer(4, 2, 8, 12);
	grid->AddGrowableCol(1, 1);

	grid->Add(new wxStaticText(cfgBox->GetStaticBox(), wxID_ANY, _("Broadcast Target:")), 0, wxALIGN_CENTER_VERTICAL);
	wxArrayString playerOpts;
	playerOpts.Add(_("Player 1 - Main TV Display (Full Screen)"));
	playerOpts.Add(_("Player 2 - Separate GamePad View (DRC Co-op Screen)"));
	playerOpts.Add(_("Player 1 - Left Half of Split-Screen"));
	playerOpts.Add(_("Player 2 - Right Half of Split-Screen"));
	m_playerChoice = new wxChoice(cfgBox->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, playerOpts);
	m_playerChoice->SetSelection(svc.targetSource.load());
	m_playerChoice->Bind(wxEVT_CHOICE, &QuestVRWindow::OnSettingsChanged, this);
	grid->Add(m_playerChoice, 1, wxEXPAND);

	grid->Add(new wxStaticText(cfgBox->GetStaticBox(), wxID_ANY, _("Quest Headset Mode:")), 0, wxALIGN_CENTER_VERTICAL);
	wxArrayString modeOpts;
	modeOpts.Add(_("Mixed Reality Passthrough (Floating Screen in Your Room)"));
	modeOpts.Add(_("Full-Screen Immersive VR Cinema (Virtual Void Theatre)"));
	m_modeChoice = new wxChoice(cfgBox->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, modeOpts);
	m_modeChoice->SetSelection(svc.defaultXrMode.load());
	m_modeChoice->Bind(wxEVT_CHOICE, &QuestVRWindow::OnSettingsChanged, this);
	grid->Add(m_modeChoice, 1, wxEXPAND);

	grid->Add(new wxStaticText(cfgBox->GetStaticBox(), wxID_ANY, _("Target Frame Rate:")), 0, wxALIGN_CENTER_VERTICAL);
	wxArrayString fpsOpts;
	fpsOpts.Add(_("60 FPS (Ultra-Low Latency)"));
	fpsOpts.Add(_("45 FPS (Balanced)"));
	fpsOpts.Add(_("30 FPS (Battery Saver)"));
	m_fpsChoice = new wxChoice(cfgBox->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, fpsOpts);
	m_fpsChoice->SetSelection(0);
	m_fpsChoice->Bind(wxEVT_CHOICE, &QuestVRWindow::OnSettingsChanged, this);
	grid->Add(m_fpsChoice, 1, wxEXPAND);

	grid->Add(new wxStaticText(cfgBox->GetStaticBox(), wxID_ANY, _("Stream Picture Quality:")), 0, wxALIGN_CENTER_VERTICAL);
	wxArrayString qualOpts;
	qualOpts.Add(_("85% - Crisp && Low Latency (Recommended)"));
	qualOpts.Add(_("95% - Maximum Visual Fidelity (5GHz / Wi-Fi 6)"));
	qualOpts.Add(_("70% - High Speed (Crowded Wi-Fi)"));
	m_qualityChoice = new wxChoice(cfgBox->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, qualOpts);
	m_qualityChoice->SetSelection(0);
	m_qualityChoice->Bind(wxEVT_CHOICE, &QuestVRWindow::OnSettingsChanged, this);
	grid->Add(m_qualityChoice, 1, wxEXPAND);

	cfgBox->Add(grid, 1, wxALL | wxEXPAND, 8);
	rootSizer->Add(cfgBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

	// 3. Broadcast Server & Quest Browser Link
	auto* streamBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Step 3: Open URL in Meta Quest Browser && Enter VR / Mixed Reality"));
	auto* urlRow = new wxBoxSizer(wxHORIZONTAL);
	urlRow->Add(new wxStaticText(streamBox->GetStaticBox(), wxID_ANY, _("Quest Browser URL:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	m_urlText = new wxTextCtrl(streamBox->GetStaticBox(), wxID_ANY, wxString::FromUTF8(svc.GetBroadcastUrl()),
							   wxDefaultPosition, wxSize(340, -1), wxTE_READONLY);
	wxFont urlFont = m_urlText->GetFont();
	urlFont.SetPointSize(urlFont.GetPointSize() + 2);
	urlFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_urlText->SetFont(urlFont);
	urlRow->Add(m_urlText, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

	auto* copyBtn = new wxButton(streamBox->GetStaticBox(), wxID_ANY, _("Copy URL"));
	copyBtn->Bind(wxEVT_BUTTON, &QuestVRWindow::OnCopyUrl, this);
	urlRow->Add(copyBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

	auto* previewBtn = new wxButton(streamBox->GetStaticBox(), wxID_ANY, _("Preview on PC"));
	previewBtn->Bind(wxEVT_BUTTON, &QuestVRWindow::OnPreviewBrowser, this);
	urlRow->Add(previewBtn, 0, wxALIGN_CENTER_VERTICAL);

	streamBox->Add(urlRow, 0, wxALL | wxEXPAND, 6);

	// WebXR 1-Click ADB Fix & Settings Guide Buttons
	auto* fixRow = new wxBoxSizer(wxHORIZONTAL);
	m_adbFixBtn = new wxButton(streamBox->GetStaticBox(), wxID_ANY, _("1-Click Fix WebXR via ADB (Forward localhost to Quest)"));
	wxFont fixFont = m_adbFixBtn->GetFont();
	fixFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_adbFixBtn->SetFont(fixFont);
	m_adbFixBtn->Bind(wxEVT_BUTTON, &QuestVRWindow::OnAdbFixWebXR, this);
	fixRow->Add(m_adbFixBtn, 0, wxRIGHT, 6);

	m_flagsGuideBtn = new wxButton(streamBox->GetStaticBox(), wxID_ANY, _("Quest Browser Settings Guide..."));
	m_flagsGuideBtn->Bind(wxEVT_BUTTON, &QuestVRWindow::OnOpenQuestFlagsGuide, this);
	fixRow->Add(m_flagsGuideBtn, 0);

	streamBox->Add(fixRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

	m_statusText = new wxStaticText(streamBox->GetStaticBox(), wxID_ANY, wxEmptyString);
	streamBox->Add(m_statusText, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

	rootSizer->Add(streamBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

	// Bottom action bar
	auto* bottomRow = new wxBoxSizer(wxHORIZONTAL);
	m_startStopBtn = new wxButton(this, wxID_ANY, _("Start Quest VR / MR Broadcast"), wxDefaultPosition, wxSize(260, 38));
	wxFont btnFont = m_startStopBtn->GetFont();
	btnFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_startStopBtn->SetFont(btnFont);
	m_startStopBtn->Bind(wxEVT_BUTTON, &QuestVRWindow::OnStartStop, this);
	bottomRow->Add(m_startStopBtn, 0, wxALL, 4);

	bottomRow->AddStretchSpacer(1);

	auto* closeBtn = new wxButton(this, wxID_CLOSE, _("Close Window (Keep Broadcast Active)"), wxDefaultPosition, wxSize(-1, 38));
	closeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
	bottomRow->Add(closeBtn, 0, wxALL, 4);

	rootSizer->Add(bottomRow, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

	SetSizer(rootSizer);
	Layout();
	Fit();
	SetMinSize(wxSize(720, 580));
	CentreOnParent();

	CemuApp::ApplyThemeToWindow(this);

	m_uiTimer = new wxTimer(this, kQuestVRTimerId);
	Bind(wxEVT_TIMER, &QuestVRWindow::OnTimer, this, kQuestVRTimerId);
	m_uiTimer->Start(500);

	RefreshUI();
}

QuestVRWindow::~QuestVRWindow()
{
	if (m_uiTimer)
	{
		m_uiTimer->Stop();
		delete m_uiTimer;
		m_uiTimer = nullptr;
	}
}

void QuestVRWindow::OnSettingsChanged(wxCommandEvent&)
{
	auto& svc = QuestBroadcastService::Instance();
	const int srcSel = m_playerChoice->GetSelection();
	svc.targetSource.store(srcSel >= 0 ? srcSel : 0);

	// If Player 2 Separate GamePad View is selected, make sure pad_open is enabled in config
	if (srcSel == 1 && !GetConfig().pad_open)
	{
		GetConfig().pad_open = true;
		g_config.Save();
		if (g_mainFrame)
			g_mainFrame->TogglePadView();
	}

	const int modeSel = m_modeChoice->GetSelection();
	svc.defaultXrMode.store(modeSel >= 0 ? modeSel : 0);

	const int fpsSel = m_fpsChoice->GetSelection();
	svc.targetFps.store(fpsSel == 1 ? 45 : (fpsSel == 2 ? 30 : 60));

	const int qualSel = m_qualityChoice->GetSelection();
	svc.jpegQuality.store(qualSel == 1 ? 95 : (qualSel == 2 ? 70 : 85));

	RefreshUI();
}

void QuestVRWindow::OnStartStop(wxCommandEvent&)
{
	auto& svc = QuestBroadcastService::Instance();
	if (svc.running.load())
	{
		svc.Stop();
	}
	else
	{
		wxCommandEvent dummy;
		OnSettingsChanged(dummy);
		svc.Start();
	}
	RefreshUI();
}

void QuestVRWindow::OnCopyUrl(wxCommandEvent&)
{
	if (wxTheClipboard->Open())
	{
		wxTheClipboard->SetData(new wxTextDataObject(m_urlText->GetValue()));
		wxTheClipboard->Close();
	}
}

void QuestVRWindow::OnPreviewBrowser(wxCommandEvent&)
{
	auto& svc = QuestBroadcastService::Instance();
	if (!svc.running.load())
	{
		wxCommandEvent dummy;
		OnSettingsChanged(dummy);
		svc.Start();
		RefreshUI();
	}
	wxLaunchDefaultBrowser(m_urlText->GetValue());
}

void QuestVRWindow::OnAdbFixWebXR(wxCommandEvent&)
{
	std::string adbPath = FindAdbExecutable();
	if (adbPath.empty())
	{
		wxMessageBox(
			_("ADB (Android Debug Bridge) was not found on your system.\n\n"
			  "To use 1-Click WebXR forwarding:\n"
			  "1. Install Meta Quest Developer Hub, SideQuest, or Android platform-tools.\n\n"
			  "Or click the 'Quest Browser Settings Guide...' button to enable WebXR over Wi-Fi!"),
			_("ADB Not Found"), wxOK | wxICON_INFORMATION, this);
		return;
	}

	auto& svc = QuestBroadcastService::Instance();
	if (!svc.running.load())
	{
		wxCommandEvent dummy;
		OnSettingsChanged(dummy);
		svc.Start();
	}

	const uint16_t currentPort = svc.port.load();
	std::string reverseArg = fmt::format("reverse tcp:{} tcp:{}", currentPort, currentPort);
	std::string revOut;
	bool ok = RunAdbCommand(reverseArg, &revOut);

	if (ok)
	{
		svc.adbForwardActive.store(true);
		m_urlText->SetValue(fmt::format("http://localhost:{}", currentPort));

		// Launch browser directly on Quest headset
		std::string launchArg = fmt::format("shell am start -a android.intent.action.VIEW -d \"http://localhost:{}\"", currentPort);
		RunAdbCommand(launchArg, nullptr);

		wxMessageBox(
			wxString::Format(
				_("WebXR 1-Click Fix Successful!\n\n"
				  "ADB reverse port forwarding is established on tcp:%u -> localhost:%u.\n"
				  "Meta Quest Browser was launched on your headset to: http://localhost:%u\n\n"
				  "Because 'localhost' is a trusted Secure Context, WebXR Mixed Reality and VR Cinema are now fully unlocked with zero security warnings!"),
				currentPort, currentPort, currentPort),
			_("WebXR Unlocked on Quest!"), wxOK | wxICON_INFORMATION, this);
	}
	else
	{
		wxString errMsg = wxString::FromUTF8(revOut);
		if (errMsg.empty())
			errMsg = _("No Quest headset detected or ADB connection failed.");

		wxMessageBox(
			wxString::Format(
				_("Could not connect to Quest headset via ADB:\n%s\n\n"
				  "How to connect:\n"
				  "1. Connect your Meta Quest to this PC using a USB cable (or enable Wireless ADB in Quest Developer settings).\n"
				  "2. Put on the headset and click 'Allow USB Debugging' if prompted.\n"
				  "3. Click '1-Click Fix WebXR via ADB' again.\n\n"
				  "Tip: You can also use the 'Quest Browser Settings Guide...' button to enable WebXR over Wi-Fi without any cables!"),
				errMsg),
			_("Quest ADB Connection"), wxOK | wxICON_WARNING, this);
	}

	RefreshUI();
}

void QuestVRWindow::OnOpenQuestFlagsGuide(wxCommandEvent&)
{
	auto& svc = QuestBroadcastService::Instance();
	std::string streamUrl = svc.GetBroadcastUrl();
	std::string flagUrl = "chrome://flags/#unsafely-treat-insecure-origin-as-secure";

	wxDialog dlg(this, wxID_ANY, _("Quest Browser WebXR Settings Guide"), wxDefaultPosition, wxSize(560, 420));
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	auto* desc = new wxStaticText(&dlg, wxID_ANY,
		_("How to unlock WebXR over Wi-Fi without ADB:\n\n"
		  "1. Inside your Meta Quest headset, open Meta Quest Browser.\n"
		  "2. In the browser URL bar, enter:\n"
		  "   chrome://flags/#unsafely-treat-insecure-origin-as-secure\n\n"
		  "3. Change the dropdown setting from 'Default' to 'Enabled'.\n\n"
		  "4. In the text box directly underneath, enter your CemuX stream URL:\n"
		  "   " + wxString::FromUTF8(streamUrl) + "\n\n"
		  "5. Click the blue 'Relaunch' button at the bottom of Quest Browser.\n\n"
		  "Done! Mixed Reality Passthrough and Full-Screen VR Cinema will now start instantly over Wi-Fi!"));
	desc->Wrap(520);
	sizer->Add(desc, 1, wxALL | wxEXPAND, 12);

	auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
	auto* copyFlagBtn = new wxButton(&dlg, wxID_ANY, _("Copy Flags URL"));
	copyFlagBtn->Bind(wxEVT_BUTTON, [flagUrl](wxCommandEvent&) {
		if (wxTheClipboard->Open()) {
			wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(flagUrl)));
			wxTheClipboard->Close();
		}
	});
	btnRow->Add(copyFlagBtn, 0, wxRIGHT, 6);

	auto* copyUrlBtn = new wxButton(&dlg, wxID_ANY, _("Copy Stream URL"));
	copyUrlBtn->Bind(wxEVT_BUTTON, [streamUrl](wxCommandEvent&) {
		if (wxTheClipboard->Open()) {
			wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(streamUrl)));
			wxTheClipboard->Close();
		}
	});
	btnRow->Add(copyUrlBtn, 0, wxRIGHT, 6);

	auto* okBtn = new wxButton(&dlg, wxID_OK, _("Close"));
	btnRow->Add(okBtn, 0);

	sizer->Add(btnRow, 0, wxALL | wxALIGN_RIGHT, 12);
	dlg.SetSizer(sizer);
	CemuApp::ApplyThemeToWindow(&dlg);
	dlg.ShowModal();
}

void QuestVRWindow::OnOpenInputSettings(wxCommandEvent&)
{
	auto dialog = std::make_unique<InputSettings2>(this);
	dialog->ShowModal();
	RefreshUI();
}

void QuestVRWindow::OnTimer(wxTimerEvent&)
{
	RefreshUI();
}

void QuestVRWindow::RefreshUI()
{
	auto& svc = QuestBroadcastService::Instance();
	m_urlText->SetValue(wxString::FromUTF8(svc.GetBroadcastUrl()));

	// Check Player 1 and Player 2 controller status
	auto& inputMgr = InputManager::instance();
	const bool hasVpad0 = (inputMgr.get_vpad_controller(0) != nullptr);
	const bool hasVpad1 = (inputMgr.get_vpad_controller(1) != nullptr);
	const bool hasWpad0 = (inputMgr.get_wpad_controller(0) != nullptr);
	const bool hasWpad1 = (inputMgr.get_wpad_controller(1) != nullptr);
	const bool hasRecentQuest0 = HasRecentQuestInput(0);
	const bool hasRecentQuest1 = HasRecentQuestInput(1);

	const bool hasP1 = hasVpad0 || hasWpad0 || hasRecentQuest0;
	const bool hasP2 = hasVpad1 || hasWpad1 || (hasVpad0 && hasWpad0) || hasRecentQuest1;

	wxString p1Desc = hasRecentQuest0 ? _("Quest VR Connected") : (hasP1 ? _("Connected && Ready") : _("Not Configured"));
	wxString p2Desc = hasRecentQuest1 ? _("Quest VR Connected") : (hasP2 ? _("Connected && Ready") : _("Not Configured"));

	wxString ctrlMsg = wxString::Format(
		_("Player 1 Controller: %s   |   Player 2 Controller: %s\n"
		  "Tip: Sync your wireless controller to the PC (or Quest via Bluetooth) before launching into VR/MR."),
		p1Desc, p2Desc);
	m_controllerStatusText->SetLabel(ctrlMsg);

	if (svc.running.load())
	{
		m_startStopBtn->SetLabel(_("Stop Quest Broadcast"));
		m_statusText->SetLabel(wxString::Format(
			_("BROADCAST LIVE on %s  -  Resolution: %dx%d  |  Frames Captured: %llu  |  Frames Streamed to Quest: %llu"),
			wxString::FromUTF8(svc.GetBroadcastUrl()),
			svc.lastCaptureW.load(),
			svc.lastCaptureH.load(),
			static_cast<unsigned long long>(svc.totalFramesCaptured.load()),
			static_cast<unsigned long long>(svc.totalFramesServed.load())));
	}
	else
	{
		m_startStopBtn->SetLabel(_("Start Quest VR / MR Broadcast"));
		m_statusText->SetLabel(
			_("Status: Broadcast stopped. Click 'Start Quest VR / MR Broadcast', then open the URL above in the Meta Quest Browser."));
	}
}
