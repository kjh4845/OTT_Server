(async function () {
  let user;
  try {
    user = await api.requireAuth();
  } catch (err) {
    if (err && err.status === 401) {
      api.redirectToLogin({ replace: true });
    }
    return;
  }

  const videoId = Number.parseInt(queryParam('v') || '', 10);
  const statusText = document.getElementById('status-text');
  const resumeBanner = document.getElementById('resume-banner');
  const resumeText = document.getElementById('resume-text');
  const resumeStartBtn = document.getElementById('resume-start');
  const resumeContinueBtn = document.getElementById('resume-continue');
  const player = document.getElementById('player');
  const logoutBtn = document.getElementById('logout-btn');
  const meta = document.getElementById('video-meta');

  if (logoutBtn) {
    logoutBtn.addEventListener('click', async () => {
      try {
        await api.post('/api/auth/logout');
      } finally {
        api.redirectToLogin({ replace: true });
      }
    });
  }

  if (!videoId) {
    statusText.textContent = 'Invalid video identifier.';
    return;
  }

  let videosData;
  let historyData;
  try {
    [videosData, historyData] = await Promise.all([
      api.get('/api/videos'),
      api.get('/api/history').catch(() => ({ history: [] })),
    ]);
  } catch (err) {
    statusText.textContent = err.message || 'Unable to load video metadata.';
    return;
  }

  const videoList = Array.isArray(videosData.videos) ? videosData.videos : [];
  const video = videoList.find((item) => item.id === videoId);
  if (!video) {
    statusText.textContent = 'Video not found.';
    return;
  }

  meta.textContent = `${video.title}${video.duration ? ` · ${Math.round(video.duration / 60)} min` : ''}`;
  player.src = `/api/videos/${videoId}/stream`;
  player.poster = `/api/videos/${videoId}/thumbnail`;

  const historyEntry = (historyData.history || []).find((item) => item.videoId === videoId);
  let resumeSeconds = 0;
  if (historyEntry && historyEntry.position && historyEntry.position > 0) {
    resumeSeconds = historyEntry.position;
  } else if (video.resumeSeconds && video.resumeSeconds > 0) {
    resumeSeconds = video.resumeSeconds;
  }

  const shouldOfferResume = Number.isFinite(resumeSeconds) && resumeSeconds >= 5;
  if (shouldOfferResume) {
    resumeText.textContent = `Continue from ${formatSeconds(resumeSeconds)}`;
    resumeBanner.style.display = 'flex';
  }

  resumeStartBtn.addEventListener('click', () => {
    resumeSeconds = 0;
    resumeBanner.style.display = 'none';
    player.currentTime = 0;
    player.play().catch(() => {});
  });
  resumeContinueBtn.addEventListener('click', () => {
    resumeBanner.style.display = 'none';
    player.currentTime = resumeSeconds;
    player.play().catch(() => {});
  });

  let lastSent = 0;
  let currentPosition = resumeSeconds;

  async function persist(position, opts = {}) {
    try {
      await fetch(`/api/history/${videoId}`, {
        method: 'POST',
        credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ position }),
        keepalive: Boolean(opts.keepalive),
      });
      statusText.textContent = `Progress saved at ${formatSeconds(position)}`;
      lastSent = Date.now();
    } catch (err) {
      console.warn('Failed to update history', err);
    }
  }

  function maybePersist(force = false) {
    const now = Date.now();
    if (!force && now - lastSent < 3000) {
      return;
    }
    const duration = Number.isFinite(player.duration) ? player.duration : 0;
    let position = player.currentTime || 0;
    if (duration > 0 && position >= duration - 5) {
      position = 0;
    }
    currentPosition = position;
    persist(position);
  }

  player.addEventListener('timeupdate', () => {
    if (player.seeking) return;
    maybePersist();
  });

  player.addEventListener('pause', () => {
    if (!player.ended) {
      maybePersist(true);
    }
  });

  player.addEventListener('ended', () => {
    currentPosition = 0;
    persist(0);
  });

  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden' && player.currentTime > 0) {
      persist(player.currentTime, { keepalive: true });
    }
  });

  window.addEventListener('beforeunload', () => {
    if (currentPosition > 0) {
      const payload = new Blob([JSON.stringify({ position: currentPosition })], {
        type: 'application/json',
      });
      navigator.sendBeacon(`/api/history/${videoId}`, payload);
    }
  });
})();
