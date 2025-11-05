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
  const welcome = document.getElementById('welcome-text');
  welcome.textContent = `Signed in as ${user.username}`;

  const status = document.getElementById('status');
  const grid = document.getElementById('videos-grid');
  const logoutBtn = document.getElementById('logout-btn');
  if (logoutBtn) {
    logoutBtn.addEventListener('click', async () => {
      try {
        await api.post('/api/auth/logout');
      } finally {
        api.redirectToLogin({ replace: true });
      }
    });
  }

  try {
    const data = await api.get('/api/videos');
    const videos = Array.isArray(data.videos) ? data.videos : [];
    if (!videos.length) {
      status.textContent = 'No videos found. Add MP4 files to the media directory to get started.';
      status.classList.add('alert');
      status.style.display = 'block';
      return;
    }
    grid.innerHTML = '';
    videos.forEach((video) => {
      const card = document.createElement('article');
      card.className = 'video-card';
      const thumb = document.createElement('img');
      thumb.src = video.thumbnailUrl;
      thumb.alt = `${video.title} thumbnail`;
      card.appendChild(thumb);

      const body = document.createElement('div');
      body.className = 'card-body';
      const title = document.createElement('h2');
      title.textContent = video.title;
      body.appendChild(title);

      if (video.description) {
        const desc = document.createElement('p');
        desc.textContent = video.description;
        body.appendChild(desc);
      }

      const resume = document.createElement('span');
      resume.className = 'resume-label';
      resume.textContent = video.resumeSeconds > 0
        ? `Resume from ${formatSeconds(video.resumeSeconds)}`
        : 'Start from the beginning';
      body.appendChild(resume);

      const controls = document.createElement('div');
      controls.style.display = 'flex';
      controls.style.gap = '0.75rem';

      const playBtn = document.createElement('a');
      playBtn.className = 'btn';
      playBtn.href = `/player.html?v=${video.id}`;
      playBtn.textContent = video.resumeSeconds > 0 ? 'Continue' : 'Play';
      controls.appendChild(playBtn);

      body.appendChild(controls);
      card.appendChild(body);
      grid.appendChild(card);
    });
  } catch (err) {
    status.textContent = err.message || 'Failed to load videos.';
    status.classList.add('error');
    status.style.display = 'block';
  }
})();
