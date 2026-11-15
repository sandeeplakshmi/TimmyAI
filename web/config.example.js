// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Timmy contributors
// Copy this file to config.js and adjust if needed. config.js is git-ignored.
// Start everything with ./start-timmy.sh, then open http://localhost:8000.
// server/timmy_server.py proxies chat to the Hermes API server and adds API_SERVER_KEY from ~/.hermes/.env,
// so no key is needed here.
window.KR_CONFIG = {
  brainUrl: "/v1/chat/completions",
  brainKey: "",            // only needed if brainUrl points straight at the Hermes API server
  model: "hermes-agent",

  // true: speak/listen with Hermes' own voice (local Whisper + Piper), same as the terminal.
  // false: use the browser's built-in speech instead.
  hermesVoice: true,

  // 3D talking avatar (AVATAR button + Male/Female in VOICE). Off by default; needs the GLB files in vendor/avatars/ (see its README).
  showAvatar: false,

  // Browser voice, used only when hermesVoice is off or unavailable. Say "list voices" to Timmy to see names.
  voice: "Daniel",   // part of a voice name, e.g. "Zoe (Premium)", "Samantha", "Evan"
  rate: 1.0,         // 0.5 (slow) to 2 (fast)
  pitch: 0.9,        // 0 (deep) to 2 (high)
};
