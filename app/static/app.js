'use strict';

const ACTIVE_STATES = ['starting','running','buffering','playable','stopping'];
const state = {
  media: [],
  session: null,
  selected: null,
  selectedDuration: null,
  positionTouched: false,
  busy: false,
  previousPlaylistUrl: null,
  optionSessionId: null,
  audioSignature: null,
  subtitleSignature: null,
  poll: null,
  token: localStorage.getItem('sylcApiToken') || '',
  tokenConfigured: false,
  setupRequired: false,
  authenticated: false,
  libraries: [],
  folderTarget: null,
  folderPath: null,
};
const $ = (id) => document.getElementById(id);
const els = {};
[
  'healthBadge','authPanel','authToken','authButton','authError','setupPanel','setupName','setupPath','setupRecursive','setupToken','setupBrowse','setupTest','setupSave','setupResult','setupError','appContent',
  'sessionTitle','statusGrid','sessionError','playbackNotice','openStream','stopButton','reportButton',
  'selectedTitle','selectedDuration','outputMode','audioStream','subtitleTrack','swapEyes','positionSlider','startInput','positionLabel','playSelected','seekButton','seekHelp',
  'refreshButton','searchInput','mediaNotice','mediaList','cleanupButton','librariesList','addLibraryButton','libraryEditor','libraryId','libraryName','libraryPath','libraryRecursive','libraryEnabled','libraryBrowse','libraryTest','librarySave','libraryCancel','libraryTestResult','tokenStatus','newToken','saveToken',
  'folderDialog','folderCurrent','folderUp','folderChoose','folderList'
].forEach((id) => { els[id] = $(id); });

function hidden(el, value) { el.classList.toggle('hidden', value); }
function isActive(session = state.session) { return Boolean(session && ACTIVE_STATES.includes(session.state)); }
function sameSelectedAsSession() { return Boolean(state.selected && state.session && state.selected.id === state.session.source?.id); }
function formatBytes(bytes) {
  const n = Number(bytes);
  if (!Number.isFinite(n)) return '—';
  const units = ['B','KiB','MiB','GiB','TiB'];
  let value = n, index = 0;
  while (value >= 1024 && index < units.length - 1) { value /= 1024; index += 1; }
  return `${value.toFixed(index < 2 ? 0 : 1)} ${units[index]}`;
}
function formatSeconds(value, alwaysHours = false) {
  const total = Math.max(0, Math.floor(Number(value) || 0));
  const h = Math.floor(total / 3600), m = Math.floor((total % 3600) / 60), s = total % 60;
  if (alwaysHours || h) return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`;
  return `${m}:${String(s).padStart(2,'0')}`;
}
function modeLabel(mode) {
  return ({
    'half-sbs':'Half-SBS', 'full-sbs':'Full-SBS',
    'half-ou':'Half-OU', 'full-ou':'Full-OU',
    'left-eye':'Left eye', 'right-eye':'Right eye',
    'anaglyph-color':'Anaglyph Color', 'anaglyph-dubois':'Anaglyph Dubois',
    'passive-rows-left-top':'Passive 4K rows · left top',
    'passive-rows-right-top':'Passive 4K rows · right top'
  })[mode] || mode || '—';
}
function audioTrackLabel(track) {
  const parts = [];
  const index = Number(track.index ?? 0);
  parts.push(`${index}: ${track.profile || String(track.format || 'audio').toUpperCase()}`);
  if (track.language) parts.push(String(track.language).toUpperCase());
  if (Number(track.channels) > 0) parts.push(`${track.channels} ch`);
  if (Number(track.sampleRate) > 0) parts.push(`${Math.round(Number(track.sampleRate) / 1000)} kHz`);
  if (track.supported !== false && String(track.profile || '').toLowerCase().includes('truehd')) {
    if (String(track.format || '').toLowerCase() === 'truehd') parts.push('native decode → AC-3 5.1');
    else if (track.embeddedAc3Core) parts.push('AC-3 core fallback');
  }
  if (track.supported === false) parts.push('unsupported');
  return parts.join(' · ');
}
function audioTracksForSelection() {
  if (!state.selected) return [];
  if (state.selected.type === 'iso' || state.selected.sourceType === 'bluray-iso') {
    return Array.isArray(state.selected.audioTracks) ? state.selected.audioTracks : [];
  }
  return [{index:0, profile:'Default MKV audio', format:'container', supported:true}];
}
function renderAudioTracks(preferred = null) {
  const tracks = audioTracksForSelection();
  const signature = JSON.stringify(tracks.map((track) => [track.index, track.profile, track.language, track.channels, track.sampleRate, track.supported, track.format, track.decodePath, track.truehdMajorSync, track.embeddedAc3Core]));
  const desired = preferred == null ? Number(els.audioStream.value || 0) : Number(preferred);
  if (signature !== state.audioSignature) {
    els.audioStream.replaceChildren();
    if (!tracks.length) {
      const option = document.createElement('option');
      option.value = '0'; option.textContent = state.selected ? 'Probe title to discover audio' : 'Select a title';
      els.audioStream.append(option);
    } else {
      tracks.forEach((track) => {
        const option = document.createElement('option');
        option.value = String(Number(track.index ?? 0));
        option.textContent = audioTrackLabel(track);
        option.disabled = track.supported === false;
        els.audioStream.append(option);
      });
    }
    state.audioSignature = signature;
  }
  const desiredOption = [...els.audioStream.options].find((option) => Number(option.value) === desired && !option.disabled);
  const firstSupported = [...els.audioStream.options].find((option) => !option.disabled);
  if (desiredOption) els.audioStream.value = desiredOption.value;
  else if (firstSupported) els.audioStream.value = firstSupported.value;
  els.audioStream.disabled = state.busy || !state.selected || !tracks.length;
}
function subtitleTrackLabel(track) {
  const parts = [];
  const language = String(track.language || 'und').toUpperCase();
  parts.push(track.title || language || 'Subtitle');
  parts.push(String(track.profile || track.format || 'subtitle').toUpperCase());
  if (track.forced) parts.push('forced');
  if (track.hearingImpaired) parts.push('SDH/HI');
  if (track.kind === 'sidecar') parts.push('sidecar');
  if (track.supported === false) parts.push(track.reason || 'unsupported');
  return parts.join(' · ');
}
function subtitleTracksForSelection() {
  return Array.isArray(state.selected?.subtitleTracks) ? state.selected.subtitleTracks : [];
}
function renderSubtitleTracks(preferred = null) {
  const tracks = subtitleTracksForSelection();
  const signature = JSON.stringify(tracks.map((track) => [track.id, track.title, track.language, track.format, track.kind, track.forced, track.hearingImpaired, track.supported, track.reason]));
  const desired = preferred == null ? String(els.subtitleTrack.value || 'off') : String(preferred || 'off');
  if (signature !== state.subtitleSignature) {
    els.subtitleTrack.replaceChildren();
    const off = document.createElement('option');
    off.value = 'off'; off.textContent = 'Off'; els.subtitleTrack.append(off);
    tracks.forEach((track) => {
      const option = document.createElement('option');
      option.value = String(track.id || '');
      option.textContent = subtitleTrackLabel(track);
      option.disabled = track.supported === false;
      els.subtitleTrack.append(option);
    });
    state.subtitleSignature = signature;
  }
  const desiredOption = [...els.subtitleTrack.options].find((option) => option.value === desired && !option.disabled);
  els.subtitleTrack.value = desiredOption ? desiredOption.value : 'off';
  els.subtitleTrack.disabled = state.busy || !state.selected;
}

function parseStartSeconds(text) {
  const value = String(text || '').trim();
  if (!value) return 0;
  if (/^\d+(?:\.\d+)?$/.test(value)) return Number(value);
  const parts = value.split(':');
  if (parts.length < 2 || parts.length > 3 || parts.some((part) => !/^\d+(?:\.\d+)?$/.test(part))) {
    throw new Error('Position must be seconds, MM:SS, or HH:MM:SS.');
  }
  const numbers = parts.map(Number);
  if (numbers.slice(1).some((part) => part >= 60)) throw new Error('Minutes and seconds must be below 60.');
  return parts.length === 3 ? numbers[0] * 3600 + numbers[1] * 60 + numbers[2] : numbers[0] * 60 + numbers[1];
}
function clampPosition(value) {
  let target = Math.max(0, Number(value) || 0);
  if (Number.isFinite(state.selectedDuration) && state.selectedDuration > 0) {
    target = Math.min(target, Math.max(0, state.selectedDuration - 0.001));
  }
  return target;
}
async function api(path, options = {}) {
  const headers = {'Content-Type':'application/json', ...(options.headers || {})};
  if (state.token) headers['X-SyLC-Token'] = state.token;
  const response = await fetch(path, { cache: 'no-store', ...options, headers });
  let data;
  try { data = await response.json(); } catch { data = {error: `HTTP ${response.status}`}; }
  if (response.status === 401) { state.authenticated = false; showAuth(); }
  if (!response.ok || data.ok === false) throw new Error(data.detail ? `${data.error}\n${data.detail}` : (data.error || `HTTP ${response.status}`));
  return data;
}

function metric(label, value) {
  const div = document.createElement('div'); div.className = 'metric';
  const span = document.createElement('span'); span.textContent = label;
  const strong = document.createElement('strong'); strong.textContent = value ?? '—';
  div.append(span, strong); return div;
}

function setPosition(value, touched = true) {
  const target = clampPosition(value);
  state.positionTouched = touched;
  els.startInput.value = formatSeconds(target, true);
  els.positionSlider.value = String(Math.floor(target));
  const total = Number.isFinite(state.selectedDuration) ? formatSeconds(state.selectedDuration) : '—';
  els.positionLabel.textContent = `${formatSeconds(target)} / ${total}`;
  renderSeekControls();
  return target;
}

function renderSeekControls() {
  const hasSelection = Boolean(state.selected && Number.isFinite(state.selectedDuration));
  els.selectedTitle.textContent = state.selected?.name || 'Select a title below';
  const isoDetail = state.selected?.sourceType === 'bluray-iso'
    ? ` · playlist ${state.selected.playlist || 'resolved'} · ${state.selected.segmentCount || '?'} clip(s)` : '';
  els.selectedDuration.textContent = hasSelection ? `Full length ${formatSeconds(state.selectedDuration)}${isoDetail}` : (state.selected ? 'Reading duration…' : 'Duration unavailable');
  els.outputMode.disabled = state.busy;
  renderAudioTracks();
  renderSubtitleTracks();
  els.swapEyes.disabled = state.busy;
  els.positionSlider.disabled = !hasSelection || state.busy;
  els.positionSlider.max = hasSelection ? String(Math.max(0, Math.floor(state.selectedDuration - 0.001))) : '0';
  els.playSelected.disabled = !hasSelection || isActive() || state.busy;
  els.seekButton.disabled = !hasSelection || !sameSelectedAsSession() || state.busy;
  document.querySelectorAll('.jump-button').forEach((button) => { button.disabled = !hasSelection || state.busy; });
  if (!hasSelection) els.positionLabel.textContent = '0:00 / —';
}

function synchronizeOutputOptionsFromSession() {
  const s = state.session;
  if (!s || state.optionSessionId === s.id) return;
  if (s.outputMode) els.outputMode.value = s.outputMode;
  renderAudioTracks(s.audioStream);
  els.audioStream.value = String(Number(s.audioStream || 0));
  renderSubtitleTracks(s.subtitleId || 'off');
  els.subtitleTrack.value = String(s.subtitleId || 'off');
  els.swapEyes.checked = Boolean(s.swapEyes);
  state.optionSessionId = s.id;
}

function renderSession() {
  const s = state.session;
  synchronizeOutputOptionsFromSession();
  els.statusGrid.replaceChildren();
  hidden(els.sessionError, true);
  if (!s) {
    els.sessionTitle.textContent = 'Idle';
    els.statusGrid.append(metric('Status','Ready for a title'));
    hidden(els.openStream, true); hidden(els.playbackNotice, true); hidden(els.stopButton, true); hidden(els.reportButton, true);
    renderSeekControls();
    return;
  }
  els.sessionTitle.textContent = s.source?.name || 'Current conversion';
  els.statusGrid.append(
    metric('State', s.state),
    metric('Elapsed', formatSeconds(s.elapsedSeconds)),
    metric('Source length', s.sourceDurationSeconds == null ? '—' : formatSeconds(s.sourceDurationSeconds)),
    metric('Output mode', modeLabel(s.outputMode)),
    metric('Audio track', String(Number(s.audioStream || 0))),
    metric('Subtitles', s.subtitleTrack ? subtitleTrackLabel(s.subtitleTrack) : 'Off'),
    metric('Eye order', s.swapEyes ? 'Swapped' : 'Normal'),
    metric('Started at', formatSeconds(s.requestedStartSeconds)),
    metric('Source position', formatSeconds(s.sourcePositionSeconds)),
    metric('Generated', formatSeconds(s.generatedDurationSeconds)),
    metric('Segments', s.segmentCount),
    metric('Output frames', s.outputPairCount),
    metric('Seek preroll', s.skippedPairCount),
    metric('Pair rate', s.pairFps == null ? '—' : `${s.pairFps.toFixed(2)}/s`),
    metric('Real-time', s.realtimeFactor == null ? (s.ffmpegSpeedFactor == null ? '—' : `${s.ffmpegSpeedFactor.toFixed(2)}×`) : `${s.realtimeFactor.toFixed(2)}×`),
    metric('Replacement', s.replacesSessionId ? `of ${s.replacesSessionId}` : 'No')
  );
  hidden(els.stopButton, !isActive(s));
  hidden(els.reportButton, false);
  hidden(els.openStream, !s.playable);
  els.openStream.dataset.playlistUrl = s.playable ? s.playlistUrl : '';

  if (s.replacesSessionId && !s.playable) {
    els.playbackNotice.textContent = 'Seek replacement is starting. The previous HLS URL remains readable while the new stream buffers.';
    hidden(els.playbackNotice, false);
  } else if (s.replacesSessionId && s.playable) {
    els.playbackNotice.textContent = 'The replacement stream is ready. Copy the new player URL and reopen it in VLC or PotPlayer.';
    hidden(els.playbackNotice, false);
  } else if (s.playable) {
    els.playbackNotice.textContent = 'Paste the copied HLS URL into VLC or PotPlayer. Browser playback may have no sound because the stream uses AC-3 audio.';
    hidden(els.playbackNotice, false);
  } else {
    hidden(els.playbackNotice, true);
  }
  if (s.lastError) { els.sessionError.textContent = s.lastError; hidden(els.sessionError, false); }

  if (sameSelectedAsSession() && !state.positionTouched && Number.isFinite(s.sourcePositionSeconds)) {
    setPosition(s.sourcePositionSeconds, false);
  }
  renderSeekControls();
}

function renderMedia() {
  const query = els.searchInput.value.trim().toLocaleLowerCase();
  const filtered = state.media.filter((item) => !query || item.relativePath.toLocaleLowerCase().includes(query));
  els.mediaList.replaceChildren();
  els.mediaNotice.textContent = `${filtered.length} of ${state.media.length} candidate MKV/MK3D or unencrypted Blu-ray 3D ISO files shown. Select one to verify MVC compatibility.`;
  filtered.forEach((item) => {
    const row = document.createElement('article');
    row.className = `media-row${state.selected?.id === item.id ? ' selected' : ''}`;
    const info = document.createElement('div');
    const title = document.createElement('h3'); title.textContent = item.name;
    const path = document.createElement('p'); path.textContent = item.relativePath;
    const meta = document.createElement('small'); meta.textContent = `${item.libraryName ? `${item.libraryName} · ` : ''}${item.type.toUpperCase()} · ${formatBytes(item.sizeBytes)}`;
    info.append(title, path, meta);
    const button = document.createElement('button'); button.className = 'button secondary'; button.type = 'button';
    button.textContent = state.selected?.id === item.id ? 'Selected' : 'Select';
    button.addEventListener('click', () => selectMedia(item));
    row.addEventListener('dblclick', () => selectMedia(item));
    row.append(info, button); els.mediaList.append(row);
  });
}

async function selectMedia(item, preservePosition = false) {
  state.selected = item;
  state.audioSignature = null;
  state.subtitleSignature = null;
  state.selectedDuration = Number.isFinite(item.durationSeconds) ? Number(item.durationSeconds) : null;
  state.positionTouched = false;
  if (!preservePosition) setPosition(0, false);
  renderMedia(); renderSeekControls();
  try {
    const data = await api('/api/media/probe', {method:'POST', body:JSON.stringify({mediaId:item.id})});
    if (!state.selected || state.selected.id !== item.id) return;
    state.selected = {...item, ...data.media};
    state.audioSignature = null;
    state.subtitleSignature = null;
    state.selectedDuration = Number(data.media.durationSeconds);
    if (sameSelectedAsSession() && !preservePosition) setPosition(state.session?.sourcePositionSeconds || state.session?.requestedStartSeconds || 0, false);
    else setPosition(parseStartSeconds(els.startInput.value), false);
    renderMedia(); renderSeekControls();
  } catch (error) {
    state.selectedDuration = null;
    alert(error.message);
    renderSeekControls();
  }
}

function synchronizeSelectionFromSession() {
  if (!state.session || state.selected) return;
  const item = state.media.find((candidate) => candidate.id === state.session.source?.id);
  if (!item) return;
  state.selected = {...item, durationSeconds: state.session.sourceDurationSeconds};
  if (Array.isArray(state.session.source?.audioTracks)) state.selected.audioTracks = state.session.source.audioTracks;
  if (Array.isArray(state.session.source?.subtitleTracks)) state.selected.subtitleTracks = state.session.source.subtitleTracks;
  state.audioSignature = null;
  state.subtitleSignature = null;
  state.selectedDuration = Number(state.session.sourceDurationSeconds);
  setPosition(state.session.sourcePositionSeconds || state.session.requestedStartSeconds || 0, false);
  renderMedia();
}

async function loadHealth() {
  try {
    const data = await api('/api/health');
    const ready = data.streamingEngineAvailable && data.streamingBinaryAvailable && data.isoSourceAdapterAvailable && data.streamingRunnerAvailable;
    els.healthBadge.textContent = ready ? 'Streaming ready' : 'Engine unavailable';
    els.healthBadge.classList.toggle('bad', !ready);
  } catch { els.healthBadge.textContent = 'Offline'; els.healthBadge.classList.add('bad'); }
}
async function loadMedia(force = false) {
  els.mediaNotice.textContent = force ? 'Rescanning…' : 'Loading…';
  try {
    const data = await api(`/api/media${force ? '?refresh=1' : ''}`);
    state.media = data.items || [];
    synchronizeSelectionFromSession();
    renderMedia();
  } catch (error) { els.mediaNotice.textContent = error.message; }
}
async function loadSession() {
  if (state.busy) return;
  try {
    const data = await api('/api/sessions/current');
    state.session = data.session;
    synchronizeSelectionFromSession();
    renderSession(); renderMedia();
  } catch (error) { els.sessionError.textContent = error.message; hidden(els.sessionError, false); }
}

async function startSelected() {
  if (!state.selected) return;
  let startSeconds;
  try { startSeconds = clampPosition(parseStartSeconds(els.startInput.value)); }
  catch (error) { alert(error.message); els.startInput.focus(); return; }
  state.busy = true; renderSeekControls();
  els.playSelected.textContent = 'Starting…';
  try {
    const data = await api('/api/sessions', {method:'POST', body:JSON.stringify({mediaId:state.selected.id, mode:els.outputMode.value, audioStream:Number(els.audioStream.value || 0), subtitleId:String(els.subtitleTrack.value || 'off'), swapEyes:els.swapEyes.checked, startSeconds})});
    state.session = data.session; state.positionTouched = false; state.previousPlaylistUrl = null; state.optionSessionId = data.session.id;
    renderSession(); renderMedia();
  } catch (error) { alert(error.message); }
  finally { state.busy = false; els.playSelected.textContent = 'Play selected title'; renderSeekControls(); }
}

async function seekCurrent(targetOverride = null) {
  if (!state.session || !sameSelectedAsSession()) return;
  let startSeconds;
  try { startSeconds = clampPosition(targetOverride == null ? parseStartSeconds(els.startInput.value) : targetOverride); }
  catch (error) { alert(error.message); els.startInput.focus(); return; }
  state.busy = true; renderSeekControls();
  els.seekButton.textContent = 'Replacing stream…';
  try {
    const data = await api('/api/sessions/current/seek', {method:'POST', body:JSON.stringify({startSeconds, mode:els.outputMode.value, audioStream:Number(els.audioStream.value || 0), subtitleId:String(els.subtitleTrack.value || 'off'), swapEyes:els.swapEyes.checked})});
    state.previousPlaylistUrl = data.previousPlaylistUrl;
    state.session = data.session;
    state.optionSessionId = data.session.id;
    state.positionTouched = false;
    setPosition(startSeconds, false);
    renderSession(); renderMedia();
  } catch (error) { alert(error.message); }
  finally { state.busy = false; els.seekButton.textContent = 'Apply mode / seek current session'; renderSeekControls(); }
}

async function quickJump(delta) {
  if (!state.selected || !Number.isFinite(state.selectedDuration)) return;
  const base = sameSelectedAsSession() && Number.isFinite(state.session?.sourcePositionSeconds)
    ? Number(state.session.sourcePositionSeconds)
    : parseStartSeconds(els.startInput.value);
  const target = clampPosition(base + Number(delta));
  setPosition(target, true);
  if (sameSelectedAsSession()) await seekCurrent(target);
}

async function copyStreamUrl() {
  const relative = els.openStream.dataset.playlistUrl;
  if (!relative) return;
  const url = new URL(relative, window.location.href).href;
  try {
    await navigator.clipboard.writeText(url);
    els.openStream.textContent = 'Player URL copied';
    setTimeout(() => { els.openStream.textContent = 'Copy player URL'; }, 1800);
  } catch { window.prompt('Copy this URL into VLC or PotPlayer:', url); }
}
async function stopSession() {
  els.stopButton.disabled = true;
  try { const data = await api('/api/sessions/current/stop', {method:'POST', body:'{}'}); state.session = data.session; renderSession(); }
  catch (error) { alert(error.message); }
  finally { els.stopButton.disabled = false; }
}
async function cleanup() {
  try { const data = await api('/api/sessions/cleanup', {method:'POST', body:'{}'}); alert(`Removed ${data.removed.length} stale session(s).`); }
  catch (error) { alert(error.message); }
}

function showAuth(message = '') {
  hidden(els.authPanel, false);
  hidden(els.setupPanel, true);
  hidden(els.appContent, true);
  els.authToken.value = state.token;
  els.authError.textContent = message;
  hidden(els.authError, !message);
}
function showSetup() {
  hidden(els.authPanel, true);
  hidden(els.setupPanel, false);
  hidden(els.appContent, true);
}
function showApp() {
  hidden(els.authPanel, true);
  hidden(els.setupPanel, true);
  hidden(els.appContent, false);
  state.authenticated = true;
}

async function authenticate() {
  state.token = els.authToken.value.trim();
  localStorage.setItem('sylcApiToken', state.token);
  try {
    await api('/api/auth/check');
    showApp();
    await loadApplication();
  } catch (error) { showAuth(error.message); }
}

function countsText(data) {
  const c = data.counts || {};
  const parts = [];
  if (c.mkv) parts.push(`${c.mkv} MKV`);
  if (c.mk3d) parts.push(`${c.mk3d} MK3D`);
  if (c.iso) parts.push(`${c.iso} ISO`);
  return parts.length ? `${parts.join(' · ')}${data.truncated ? ' · count limited' : ''}` : 'Folder is readable; no supported files were found yet.';
}

async function testPath(pathEl, recursiveEl, resultEl) {
  resultEl.textContent = 'Testing folder…';
  const data = await api('/api/libraries/test', {method:'POST', body:JSON.stringify({path:pathEl.value, recursive:recursiveEl.checked})});
  pathEl.value = data.path;
  resultEl.textContent = `✓ Readable · ${countsText(data)}`;
  return data;
}

async function finishSetup() {
  hidden(els.setupError, true);
  els.setupSave.disabled = true;
  try {
    await testPath(els.setupPath, els.setupRecursive, els.setupResult);
    const token = els.setupToken.value.trim();
    const data = await api('/api/setup', {method:'POST', body:JSON.stringify({name:els.setupName.value, path:els.setupPath.value, recursive:els.setupRecursive.checked, enabled:true, apiToken:token})});
    state.token = token;
    localStorage.setItem('sylcApiToken', token);
    state.tokenConfigured = data.tokenConfigured;
    showApp();
    await loadApplication();
  } catch (error) {
    els.setupError.textContent = error.message;
    hidden(els.setupError, false);
  } finally { els.setupSave.disabled = false; }
}

function openLibraryEditor(library = null) {
  hidden(els.libraryEditor, false);
  els.libraryId.value = library?.id || '';
  els.libraryName.value = library?.name || '';
  els.libraryPath.value = library?.path || '';
  els.libraryRecursive.checked = library?.recursive ?? true;
  els.libraryEnabled.checked = library?.enabled ?? true;
  els.libraryTestResult.textContent = '';
  els.libraryName.focus();
}
function closeLibraryEditor() { hidden(els.libraryEditor, true); els.libraryId.value = ''; }

function renderLibraries() {
  els.librariesList.replaceChildren();
  state.libraries.forEach((library) => {
    const row = document.createElement('article'); row.className = 'library-row';
    const info = document.createElement('div');
    const title = document.createElement('h3'); title.textContent = library.name;
    const path = document.createElement('p'); path.textContent = library.path;
    const status = document.createElement('small');
    const access = library.readable ? 'Readable' : (library.exists ? 'Not readable' : 'Missing');
    let count = 'Not scanned';
    if (library.indexedFiles != null) {
      const c = library.fileCounts || {};
      const parts = [];
      if (c.mkv) parts.push(`${c.mkv} MKV`);
      if (c.mk3d) parts.push(`${c.mk3d} MK3D`);
      if (c.iso) parts.push(`${c.iso} ISO`);
      count = parts.length ? parts.join(' · ') : '0 supported files';
    }
    status.textContent = `${library.enabled ? 'Enabled' : 'Disabled'} · ${access} · ${library.recursive ? 'Recursive' : 'Top folder only'} · ${count}`;
    info.append(title,path,status);
    const actions = document.createElement('div'); actions.className = 'actions';
    const edit = document.createElement('button'); edit.className='button secondary'; edit.type='button'; edit.textContent='Edit'; edit.addEventListener('click',()=>openLibraryEditor(library));
    const scan = document.createElement('button'); scan.className='button secondary'; scan.type='button'; scan.textContent='Rescan'; scan.addEventListener('click',()=>rescanLibrary(library.id));
    const remove = document.createElement('button'); remove.className='button danger'; remove.type='button'; remove.textContent='Remove'; remove.addEventListener('click',()=>removeLibrary(library));
    actions.append(edit,scan,remove); row.append(info,actions); els.librariesList.append(row);
  });
}
async function loadLibraries(refresh = false) {
  const data = await api(`/api/libraries${refresh ? '?refresh=1' : ''}`);
  state.libraries = data.libraries || [];
  renderLibraries();
}
async function saveLibrary() {
  try {
    await testPath(els.libraryPath, els.libraryRecursive, els.libraryTestResult);
    const body = JSON.stringify({name:els.libraryName.value, path:els.libraryPath.value, recursive:els.libraryRecursive.checked, enabled:els.libraryEnabled.checked});
    const id = els.libraryId.value;
    await api(id ? `/api/libraries/${id}` : '/api/libraries', {method:id?'PUT':'POST', body});
    closeLibraryEditor();
    await loadLibraries(true); await loadMedia(true);
  } catch (error) { els.libraryTestResult.textContent = `✗ ${error.message}`; }
}
async function rescanLibrary(id) {
  try { await api(`/api/libraries/${id}/rescan`, {method:'POST', body:'{}'}); await loadLibraries(false); await loadMedia(true); }
  catch (error) { alert(error.message); }
}
async function removeLibrary(library) {
  if (!confirm(`Remove “${library.name}” from SyLC? No files will be deleted.`)) return;
  try { await api(`/api/libraries/${library.id}`, {method:'DELETE'}); await loadLibraries(true); await loadMedia(true); }
  catch (error) { alert(error.message); }
}

async function updateToken() {
  const newToken = els.newToken.value.trim();
  if (!confirm(newToken ? 'Replace the API token? Other clients will need the new token.' : 'Disable API-token authentication? Use this only on a trusted LAN or VPN.')) return;
  try {
    const data = await api('/api/settings/token', {method:'POST', body:JSON.stringify({apiToken:newToken})});
    state.token = newToken; state.tokenConfigured = data.tokenConfigured;
    localStorage.setItem('sylcApiToken', newToken); els.newToken.value=''; renderTokenStatus();
  } catch (error) { alert(error.message); }
}
function renderTokenStatus() { els.tokenStatus.textContent = state.tokenConfigured ? 'API-token authentication is enabled.' : 'Authentication is disabled. Trusted LAN/VPN use only.'; }

async function openFolderBrowser(target) {
  state.folderTarget = target;
  state.folderPath = null;
  els.folderDialog.showModal();
  await browseFolder(null);
}
async function browseFolder(path) {
  try {
    const data = await api(`/api/filesystem/directories${path ? `?path=${encodeURIComponent(path)}` : ''}`);
    state.folderPath = data.path;
    els.folderCurrent.textContent = data.path || 'Choose an allowed server root';
    hidden(els.folderUp, !data.parent);
    els.folderUp.dataset.path = data.parent || '';
    hidden(els.folderChoose, !data.path);
    els.folderList.replaceChildren();
    (data.directories || []).forEach((dir) => {
      const button = document.createElement('button'); button.className='folder-row'; button.type='button';
      button.textContent = `${dir.readable ? '📁' : '🔒'} ${dir.name}`;
      button.disabled = !dir.readable;
      button.addEventListener('click',()=>browseFolder(dir.path));
      els.folderList.append(button);
    });
  } catch (error) { els.folderCurrent.textContent = error.message; }
}
function chooseFolder() {
  if (state.folderTarget && state.folderPath) state.folderTarget.value = state.folderPath;
  els.folderDialog.close();
}

async function downloadReport(event) {
  event.preventDefault();
  try {
    const headers = {}; if (state.token) headers['X-SyLC-Token'] = state.token;
    const response = await fetch('/api/sessions/current/report', {headers});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const blob = await response.blob(); const url = URL.createObjectURL(blob);
    const a = document.createElement('a'); a.href=url; a.download=`sylc-session-${state.session?.id || 'current'}-report.txt`; a.click();
    setTimeout(()=>URL.revokeObjectURL(url),1000);
  } catch (error) { alert(`Could not download report: ${error.message}`); }
}

async function loadApplication() {
  await Promise.all([loadHealth(), loadLibraries(true), loadMedia(), loadSession()]);
  renderTokenStatus();
  if (state.poll) clearInterval(state.poll);
  state.poll = setInterval(loadSession, 1000);
}

async function initialize() {
  try {
    const status = await api('/api/setup/status');
    state.setupRequired = status.setupRequired; state.tokenConfigured = status.tokenConfigured;
    if (state.setupRequired) { showSetup(); return; }
    if (state.tokenConfigured) {
      if (!state.token) { showAuth(); return; }
      try { await api('/api/auth/check'); } catch { showAuth('The saved token was not accepted.'); return; }
    }
    showApp(); await loadApplication();
  } catch (error) { showAuth(error.message); }
}

els.authButton.addEventListener('click', authenticate);
els.authToken.addEventListener('keydown', (event)=>{if(event.key==='Enter') authenticate();});
els.setupBrowse.addEventListener('click',()=>openFolderBrowser(els.setupPath));
els.setupTest.addEventListener('click',async()=>{try{await testPath(els.setupPath,els.setupRecursive,els.setupResult);}catch(error){els.setupResult.textContent=`✗ ${error.message}`;}});
els.setupSave.addEventListener('click', finishSetup);
els.addLibraryButton.addEventListener('click',()=>openLibraryEditor());
els.libraryBrowse.addEventListener('click',()=>openFolderBrowser(els.libraryPath));
els.libraryTest.addEventListener('click',async()=>{try{await testPath(els.libraryPath,els.libraryRecursive,els.libraryTestResult);}catch(error){els.libraryTestResult.textContent=`✗ ${error.message}`;}});
els.librarySave.addEventListener('click', saveLibrary);
els.libraryCancel.addEventListener('click', closeLibraryEditor);
els.saveToken.addEventListener('click', updateToken);
els.folderUp.addEventListener('click',()=>browseFolder(els.folderUp.dataset.path));
els.folderChoose.addEventListener('click', chooseFolder);


els.refreshButton.addEventListener('click', () => loadMedia(true));
els.searchInput.addEventListener('input', renderMedia);
els.positionSlider.addEventListener('input', () => setPosition(Number(els.positionSlider.value), true));
els.startInput.addEventListener('change', () => {
  try { setPosition(parseStartSeconds(els.startInput.value), true); }
  catch (error) { alert(error.message); }
});
els.outputMode.addEventListener('change', renderSeekControls);
els.audioStream.addEventListener('change', renderSeekControls);
els.subtitleTrack.addEventListener('change', renderSeekControls);
els.swapEyes.addEventListener('change', renderSeekControls);
els.playSelected.addEventListener('click', startSelected);
els.seekButton.addEventListener('click', () => seekCurrent());
document.querySelectorAll('.jump-button').forEach((button) => button.addEventListener('click', () => quickJump(Number(button.dataset.delta))));
els.openStream.addEventListener('click', copyStreamUrl);
els.reportButton.addEventListener('click', downloadReport);
els.stopButton.addEventListener('click', stopSession);
els.cleanupButton.addEventListener('click', cleanup);

initialize();
