#include "gui/wxgui.h"
#include "gui/QuestVRWindow.h"
#include "gui/CemuApp.h"
#include "gui/MainWindow.h"
#include "gui/input/InputSettings2.h"
#include "input/InputManager.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"

#include <wx/clipbrd.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/utils.h>

#include <atomic>
#include <chrono>
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
			return fmt::format("http://{}:{}", DetectLanIp(), port.load());
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

			CLSID jpegClsid{};
			GetJpegEncoderClsid(jpegClsid);

			while (running.load())
			{
				const auto frameStart = std::chrono::steady_clock::now();

				const int srcMode = targetSource.load();
				HWND hwndTarget = nullptr;
				if (srcMode == 1)
				{
					hwndTarget = reinterpret_cast<HWND>(padHwnd.load());
					if (!hwndTarget || !IsWindow(hwndTarget) || !IsWindowVisible(hwndTarget))
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
					int srcW = rc.right - rc.left;
					int srcH = rc.bottom - rc.top;

					if (srcW >= 64 && srcH >= 64 && !IsIconic(hwndTarget))
					{
						int cropX = 0;
						int cropW = srcW;
						if (srcMode == 2) // Left half (P1 Split)
						{
							cropX = 0;
							cropW = srcW / 2;
						}
						else if (srcMode == 3) // Right half (P2 Split)
						{
							cropX = srcW / 2;
							cropW = srcW - cropX;
						}

						// Cap output resolution to 1920x1080 for ultra-low latency 60fps streaming
						int outW = cropW;
						int outH = srcH;
						if (outW > 1920)
						{
							outH = (outH * 1920) / outW;
							outW = 1920;
						}
						outW = std::max(64, outW & ~1);
						outH = std::max(64, outH & ~1);

						HDC hdcWin = GetDC(hwndTarget);
						if (hdcWin)
						{
							HDC hdcFull = CreateCompatibleDC(hdcWin);
							HBITMAP hbmFull = CreateCompatibleBitmap(hdcWin, srcW, srcH);
							HGDIOBJ oldFull = SelectObject(hdcFull, hbmFull);

							BOOL ok = PrintWindow(hwndTarget, hdcFull, PW_RENDERFULLCONTENT | PW_CLIENTONLY);
							if (!ok)
								ok = BitBlt(hdcFull, 0, 0, srcW, srcH, hdcWin, 0, 0, SRCCOPY);

							HBITMAP hbmToEncode = hbmFull;
							HDC hdcScaled = nullptr;
							HBITMAP hbmScaled = nullptr;
							HGDIOBJ oldScaled = nullptr;

							if (cropX != 0 || cropW != srcW || outW != srcW || outH != srcH)
							{
								hdcScaled = CreateCompatibleDC(hdcWin);
								hbmScaled = CreateCompatibleBitmap(hdcWin, outW, outH);
								oldScaled = SelectObject(hdcScaled, hbmScaled);
								SetStretchBltMode(hdcScaled, HALFTONE);
								StretchBlt(hdcScaled, 0, 0, outW, outH, hdcFull, cropX, 0, cropW, srcH, SRCCOPY);
								hbmToEncode = hbmScaled;
							}

							{
								Gdiplus::Bitmap bmp(hbmToEncode, nullptr);
								IStream* memStream = nullptr;
								if (CreateStreamOnHGlobal(nullptr, TRUE, &memStream) == S_OK && memStream)
								{
									ULONG qual = static_cast<ULONG>(std::clamp(jpegQuality.load(), 40, 100));
									Gdiplus::EncoderParameters encParams{};
									encParams.Count = 1;
									encParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
									encParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
									encParams.Parameter[0].NumberOfValues = 1;
									encParams.Parameter[0].Value = &qual;

									if (bmp.Save(memStream, &jpegClsid, &encParams) == Gdiplus::Ok)
									{
										HGLOBAL hMem = nullptr;
										if (GetHGlobalFromStream(memStream, &hMem) == S_OK && hMem)
										{
											const SIZE_T bytes = GlobalSize(hMem);
											const void* ptr = GlobalLock(hMem);
											if (ptr && bytes > 0)
											{
												std::vector<uint8_t> encoded(
													 static_cast<const uint8_t*>(ptr),
													 static_cast<const uint8_t*>(ptr) + bytes);
												GlobalUnlock(hMem);

												{
													std::lock_guard<std::mutex> lk(frameMutex);
													latestJpeg = std::move(encoded);
													++latestSeq;
												}
												++totalFramesCaptured;
												lastCaptureW.store(outW);
												lastCaptureH.store(outH);
											}
										}
									}
									memStream->Release();
								}
							}

							if (hdcScaled)
							{
								SelectObject(hdcScaled, oldScaled);
								DeleteObject(hbmScaled);
								DeleteDC(hdcScaled);
							}
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
.ctrl-badge{padding:4px 10px;border-radius:999px;font-size:12px;font-weight:700;background:#1e293b;color:#94a3b8;border:1px solid #475569}
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
#overlayTip{position:absolute;bottom:14px;left:50%;transform:translateX(-50%);background:rgba(15,23,42,0.85);border:1px solid #334155;padding:7px 16px;border-radius:999px;font-size:13px;color:#cbd5e1;pointer-events:none}
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
    <span class="ctrl-badge" id="padBadge">🎮 Sync Wireless Controller</span>
  </div>
  <div class="actions">
    <button class="btn-mr" id="btnMR">🌐 Mixed Reality (Passthrough)</button>
    <button class="btn-vr" id="btnVR">🥽 Full-Screen Immersive VR</button>
    <button class="btn-fs" id="btnFS">🖥️ 2D Fullscreen</button>
  </div>
</div>
<div id="stage">
  <img id="screen2d" alt="CemuX Live Broadcast"/>
  <canvas id="xrCanvas"></canvas>
  <div id="overlayTip">In VR/MR: Thumbstick Up/Down = Screen Size • Thumbstick Left/Right = Distance • A/X Button = Recenter</div>
</div>
<script>
const img2d = document.getElementById('screen2d');
const statsText = document.getElementById('statsText');
const padBadge = document.getElementById('padBadge');
const xrCanvas = document.getElementById('xrCanvas');
const defaultMode = ")HTML" << modeHint << R"HTML(";

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
  nextImg.src = `/frame.jpg?t=${ performance.now() }`;
}
pollFrame();

function checkGamepads() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  let activeName = "";
  for (let i = 0; i < pads.length; i++) {
    if (pads[i] && pads[i].connected) {
      activeName = pads[i].id;
      break;
    }
  }
  if (activeName) {
    padBadge.textContent = "🎮 Controller Ready";
    padBadge.classList.add("ok");
  } else {
    padBadge.textContent = "🎮 Wireless Controller (PC / Quest)";
    padBadge.classList.remove("ok");
  }
}
setInterval(checkGamepads, 1000);

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
    alert("WebXR is only available inside the Meta Quest Browser (open this URL on your Quest headset). Using Fullscreen Theatre mode on this device.");
    document.getElementById('stage').requestFullscreen();
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
    alert("Could not start WebXR session: " + e.message);
  }
}

function onXRFrame(time, frame, isAR) {
  const session = frame.session;
  session.requestAnimationFrame((t, f) => onXRFrame(t, f, isAR));
  const pose = frame.getViewerPose(refSpace);
  if (!pose) return;

  // Read Quest Touch / Gamepad thumbsticks in XR session to resize/move/recenter the screen
  for (const src of session.inputSources) {
    if (src.gamepad) {
      const axes = src.gamepad.axes;
      if (axes.length >= 4) {
        if (Math.abs(axes[3]) > 0.2) screenWidth = Math.max(1.2, Math.min(6.5, screenWidth - axes[3] * 0.03));
        if (Math.abs(axes[2]) > 0.2) screenDist  = Math.max(1.2, Math.min(5.5, screenDist  + axes[2] * 0.03));
      }
      if (src.gamepad.buttons[0] && src.gamepad.buttons[0].pressed) {
        needRecenter = true;
      }
    }
  }

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

			if (req.rfind("GET /frame.jpg", 0) == 0)
			{
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

QuestVRWindow::QuestVRWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Quest VR & Mixed Reality Broadcast Studio (Player 1 / Player 2)"),
			   wxDefaultPosition, wxSize(680, 560), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	auto& svc = QuestBroadcastService::Instance();

	auto* rootSizer = new wxBoxSizer(wxVERTICAL);

	// Header banner
	auto* headerBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Meta Quest 2 / Quest 3 / Quest Pro Wireless Broadcast"));
	auto* introText = new wxStaticText(
		headerBox->GetStaticBox(), wxID_ANY,
		_("Stream CemuX directly to a Meta Quest headset over local Wi-Fi in either Mixed Reality Passthrough "
		  "(floating screen in your room) or Full-Screen Immersive VR Cinema — for either Player 1 or Player 2!"));
	introText->Wrap(620);
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
	auto* cfgBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Step 2: Choose Target Player & Quest VR / Mixed Reality Mode"));
	auto* grid = new wxFlexGridSizer(4, 2, 8, 12);
	grid->AddGrowableCol(1, 1);

	grid->Add(new wxStaticText(cfgBox->GetStaticBox(), wxID_ANY, _("Broadcast Target:")), 0, wxALIGN_CENTER_VERTICAL);
	wxArrayString playerOpts;
	playerOpts.Add(_("Player 1 — Main TV Display (Full Screen)"));
	playerOpts.Add(_("Player 2 — Separate GamePad View (DRC Co-op Screen)"));
	playerOpts.Add(_("Player 1 — Left Half of Split-Screen"));
	playerOpts.Add(_("Player 2 — Right Half of Split-Screen"));
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
	qualOpts.Add(_("85% — Crisp & Low Latency (Recommended)"));
	qualOpts.Add(_("95% — Maximum Visual Fidelity (5GHz / Wi-Fi 6)"));
	qualOpts.Add(_("70% — High Speed (Crowded Wi-Fi)"));
	m_qualityChoice = new wxChoice(cfgBox->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, qualOpts);
	m_qualityChoice->SetSelection(0);
	m_qualityChoice->Bind(wxEVT_CHOICE, &QuestVRWindow::OnSettingsChanged, this);
	grid->Add(m_qualityChoice, 1, wxEXPAND);

	cfgBox->Add(grid, 1, wxALL | wxEXPAND, 8);
	rootSizer->Add(cfgBox, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

	// 3. Broadcast Server & Quest Browser Link
	auto* streamBox = new wxStaticBoxSizer(wxVERTICAL, this, _("Step 3: Open URL in Meta Quest Browser & Enter VR / Mixed Reality"));
	auto* urlRow = new wxBoxSizer(wxHORIZONTAL);
	urlRow->Add(new wxStaticText(streamBox->GetStaticBox(), wxID_ANY, _("Quest Browser URL:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	m_urlText = new wxTextCtrl(streamBox->GetStaticBox(), wxID_ANY, wxString::FromUTF8(svc.GetBroadcastUrl()),
							   wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
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
	SetMinSize(wxSize(640, 520));
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
	const bool hasP1 = hasVpad0 || hasWpad0;
	const bool hasP2 = hasVpad1 || hasWpad1 || (hasVpad0 && hasWpad0);

	wxString ctrlMsg = wxString::Format(
		_("Player 1 Controller: %s   |   Player 2 Controller: %s\n"
		  "Tip: Sync your wireless controller to the PC (or Quest via Bluetooth) before launching into VR/MR."),
		hasP1 ? _("Connected && Ready") : _("Not Configured"),
		hasP2 ? _("Connected && Ready") : _("Not Configured"));
	m_controllerStatusText->SetLabel(ctrlMsg);

	if (svc.running.load())
	{
		m_startStopBtn->SetLabel(_("Stop Quest Broadcast"));
		m_statusText->SetLabel(wxString::Format(
			_("BROADCAST LIVE on %s  —  Resolution: %dx%d  |  Frames Captured: %llu  |  Frames Streamed to Quest: %llu"),
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
