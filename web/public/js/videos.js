// 비디오 라이브러리 페이지: 무한 스크롤 목록 + 검색 기능
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
  if (welcome) {
    welcome.textContent = `Signed in as ${user.username}`;
  }

  const status = document.getElementById('status');
  const grid = document.getElementById('videos-grid');
  const loader = document.getElementById('feed-loader');
  const endMessage = document.getElementById('feed-end');
  const sentinel = document.getElementById('scroll-sentinel');
  const searchForm = document.getElementById('search-form');
  const searchInput = document.getElementById('video-search');
  const clearBtn = document.getElementById('search-clear');
  const logoutBtn = document.getElementById('logout-btn');

  const state = {
    cursor: 0,
    limit: 12,
    isLoading: false,
    hasMore: true,
    query: '',
  };

  if (logoutBtn) {
    logoutBtn.addEventListener('click', async () => {
      try {
        await api.post('/api/auth/logout');
      } finally {
        api.redirectToLogin({ replace: true });
      }
    });
  }

  if (searchForm) {
    searchForm.addEventListener('submit', (event) => {
      event.preventDefault();
      startSearch();
    });
  }
  if (clearBtn) {
    clearBtn.addEventListener('click', () => {
      if (searchInput) {
        searchInput.value = '';
      }
      if (state.query) {
        startSearch();
      } else {
        updateClearButton();
      }
    });
  }
  if (searchInput) {
    searchInput.addEventListener('input', updateClearButton);
  }
  updateClearButton();

  if (sentinel && 'IntersectionObserver' in window) {
    // 스크롤 최하단 근처에 도달하면 다음 페이지를 불러온다.
    const observer = new IntersectionObserver((entries) => {
      if (!entries[0].isIntersecting) {
        return;
      }
      loadPage();
    }, { rootMargin: '400px 0px' });
    observer.observe(sentinel);
  }

  await loadPage({ reset: true });

  // 서버에서 일정 개수의 비디오를 가져와 렌더링한다.
  async function loadPage({ reset = false } = {}) {
    if (state.isLoading) {
      return;
    }
    if (!state.hasMore && !reset) {
      showEndMessage();
      return;
    }
    state.isLoading = true;
    hideStatus();
    if (loader) {
      loader.style.display = 'flex';
    }
    if (reset) {
      state.cursor = 0;
      state.hasMore = true;
      if (grid) {
        grid.innerHTML = '';
      }
      if (endMessage) {
        endMessage.style.display = 'none';
      }
    }
    try {
      // limit/cursor를 쿼리로 전달해 서버에서 페이지네이션
      const params = new URLSearchParams({ limit: state.limit.toString(), cursor: state.cursor.toString() });
      if (state.query) {
        params.set('q', state.query);
      }
      const data = await api.get(`/api/videos?${params.toString()}`);
      const videos = Array.isArray(data.videos) ? data.videos : [];
      renderVideos(videos);
      const nextCursor = typeof data.nextCursor === 'number' ? data.nextCursor : state.cursor + videos.length;
      state.cursor = nextCursor;
      state.hasMore = typeof data.hasMore === 'boolean' ? data.hasMore : videos.length >= state.limit;
      if (!videos.length && state.cursor === 0) {
        const message = state.query
          ? `No videos matched "${state.query}".`
          : 'No videos found. Add MP4 files to the media directory to get started.';
        showStatus(message, false);
      } else {
        hideStatus();
      }
      showEndMessage();
    } catch (err) {
      showStatus(err.message || 'Failed to load videos.', true);
    } finally {
      state.isLoading = false;
      if (loader) {
        loader.style.display = 'none';
      }
    }
  }

  function renderVideos(videos) {
    if (!grid || !Array.isArray(videos)) {
      return;
    }
    videos.forEach((video) => {
      // 하나의 비디오 카드를 구성하고 클릭 시 플레이어 페이지로 이동시킨다.
      const card = document.createElement('article');
      card.className = 'video-card video-card--link';
      card.tabIndex = 0;
      card.setAttribute('role', 'link');
      const navigateToPlayer = () => {
        window.location.href = `/player.html?v=${video.id}`;
      };
      card.addEventListener('click', (event) => {
        if (event.target.closest('.btn')) {
          return;
        }
        navigateToPlayer();
      });
      card.addEventListener('keydown', (event) => {
        if (event.key === 'Enter' || event.key === ' ') {
          event.preventDefault();
          navigateToPlayer();
        }
      });
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
  }

  function showStatus(message, isError) {
    if (!status) return;
    status.textContent = message;
    status.classList.toggle('error', Boolean(isError));
    status.style.display = 'block';
  }

  function hideStatus() {
    if (!status) return;
    status.style.display = 'none';
  }

  function showEndMessage() {
    if (!endMessage) return;
    if (!state.hasMore && grid && grid.children.length > 0) {
      endMessage.style.display = 'block';
    } else {
      endMessage.style.display = 'none';
    }
  }

  function startSearch() {
    if (!searchInput) {
      return;
    }
    state.query = searchInput.value.trim();
    updateClearButton();
    loadPage({ reset: true });
  }

  function updateClearButton() {
    if (!clearBtn || !searchInput) return;
    clearBtn.style.visibility = searchInput.value.trim() ? 'visible' : 'hidden';
  }
})();
