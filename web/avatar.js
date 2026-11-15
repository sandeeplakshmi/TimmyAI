// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Timmy contributors
// Timmy's 3D talking avatar (TalkingHead + three.js, vendored in ./vendor, works offline).
// index.html uses it through window.KRAvatar: start(), say(text), stop(), setState(state), shutdown().
// Lip-sync: server/timmy_server.py returns Piper audio plus estimated word timings; TalkingHead turns the
// words into visemes (lipsync-en) and plays the audio in step with them.
import { TalkingHead } from "./vendor/talkinghead/talkinghead.mjs";

// Both are TalkingHead's sample avatars, not shipped here (see vendor/avatars/README.md).
const AVATARS = {
  male: { url: "./vendor/avatars/avatarsdk.glb", body: "M",     // Avatar SDK sample
          // Settings from TalkingHead's own demo for this model (needs the backported retargeter.mjs):
          // straighten the neck so he doesn't look down, relax the shoulders, scale to eye level.
          retarget: {
            Neck: { z: -0.01, rx: -0.15 }, Neck1: { z: -0.01, rx: -0.15 }, Neck2: { z: -0.01, rx: -0.15 },
            LeftShoulder: { rz: -0.3 }, RightShoulder: { rz: 0.3 },
            scaleToEyesLevel: 1.0, origin: { y: -0.1 },
          },
          baseline: { headRotateX: -0.04, eyeBlinkLeft: 0.05, eyeBlinkRight: 0.05 } },
  female: { url: "./vendor/avatars/brunette.glb", body: "F" },  // Ready Player Me sample, CC BY-NC 4.0
};
function chosenAvatar() {
  try { return AVATARS[localStorage.getItem("kr.avatarModel")] ? localStorage.getItem("kr.avatarModel") : "male"; }
  catch { return "male"; }
}
let head = null, node = null, ready = null;

async function start() {
  if (ready) return ready;
  node = document.getElementById("avatar");
  ready = (async () => {
    const a = AVATARS[chosenAvatar()];
    head = new TalkingHead(node, {
      ...(a.view || {}),               // per-avatar camera framing (applied by showAvatar's own setView)
      ...(innerWidth < 700 ? { cameraDistance: ((a.view || {}).cameraDistance || 0) + 0.45 } : {}),   // phones: a bit further back

      ttsEndpoint: "",                 // no cloud TTS: audio comes from Piper via server/timmy_server.py
      lipsyncModules: ["en"], lipsyncLang: "en",
      cameraView: "upper",             // head to chest
      cameraRotateEnable: false, cameraPanEnable: false, cameraZoomEnable: false,
      modelFPS: 30, avatarMood: "neutral",
      lightAmbientIntensity: 2, lightDirectIntensity: 25,
    });
    const loading = document.getElementById("avatarload");
    await head.showAvatar({ url: a.url, body: a.body, baseline: a.baseline, retarget: a.retarget,
                            avatarMood: "neutral", lipsyncLang: "en" },
      ev => { if (loading && ev.lengthComputable) loading.textContent = `Loading avatar… ${Math.round(ev.loaded / ev.total * 100)}%`; });
    return head;
  })();
  return ready;
}

// Speak text with Piper + lip-sync. Resolves when playback finishes or is stopped.
async function say(text, onStart) {
  await start();
  const r = await fetch("/voice/speak-lipsync", { method: "POST", headers: { "Content-Type": "application/json" },
                                                  body: JSON.stringify({ text }) });
  if (!r.ok) throw new Error("lipsync tts " + r.status);
  const j = await r.json();
  const bytes = Uint8Array.from(atob(j.audio), c => c.charCodeAt(0));
  if (head.audioCtx.state === "suspended") await head.audioCtx.resume();
  const audio = await head.audioCtx.decodeAudioData(bytes.buffer);
  head.speakAudio({ audio, words: j.words, wtimes: j.wtimes, wdurations: j.wdurations });
  onStart?.();
  if (Math.random() < 0.5) head.speakWithHands?.(400, 0.4);
  await new Promise(res => {
    const t0 = performance.now();
    (function wait() {
      // speakAudio queues the line; wait for it to start, then for it to end
      if (performance.now() - t0 > 300 && !head.isSpeaking) return res();
      setTimeout(wait, 100);
    })();
  });
}

function stop() { head?.stopSpeaking(); }

// Map Timmy's states onto the avatar's attention.
function setState(state) {
  if (!head) return;
  if (state === "listening") { head.lookAtCamera(400); head.setMood("happy"); }
  else if (state === "thinking") { head.setMood("neutral"); head.lookAt(innerWidth * 0.75, innerHeight * 0.1, 900); }
  else if (state === "speaking") head.lookAtCamera(300);
  else { head.setMood("neutral"); head.makeEyeContact(1500); }
}

function shutdown() { head?.stop(); }
function reset() { try { head?.stop(); node && (node.innerHTML = ""); } catch {} head = null; ready = null; }   // allow a retry

window.KRAvatar = { start, say, stop, setState, shutdown, reset, get loaded() { return !!head?.armature; }, get head() { return head; } };
window.dispatchEvent(new Event("kravatar-ready"));
