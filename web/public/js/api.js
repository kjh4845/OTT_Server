// 클라이언트 측에서 재사용하는 fetch 래퍼와 라우팅 유틸 모음
const api = (() => {
  const baseHeaders = { 'Content-Type': 'application/json' };

  // SPA가 아니므로 단순히 pathname을 정규화해 중복 이동을 방지한다.
  function normalizePath(path) {
    if (!path) {
      return '/';
    }
    try {
      const url = new URL(path, window.location.origin);
      let pathname = url.pathname || '/';
      if (pathname === '/') {
        pathname = '/index.html';
      }
      return pathname + (url.search || '') + (url.hash || '');
    } catch (err) {
      return path;
    }
  }

  // 동일한 경로로 중복 이동하지 않도록 하고 history replace 지원
  function navigateTo(path, options = {}) {
    if (typeof path !== 'string' || path.length === 0) {
      return;
    }
    const target = normalizePath(path);
    const current = normalizePath(
      (window.location.pathname || '/') +
      (window.location.search || '') +
      (window.location.hash || '')
    );
    if (target === current) {
      return;
    }
    if (window.__ottNavigatingTo === target) {
      return;
    }
    window.__ottNavigatingTo = target;
    const href = target.startsWith('/') ? target : `/${target}`;
    if (options.replace) {
      window.location.replace(href);
    } else {
      window.location.assign(href);
    }
  }

  // 기본 헤더/credentials를 설정한 fetch 래퍼
  async function request(path, options = {}) {
    const opts = {
      credentials: 'include',
      headers: { ...baseHeaders, ...(options.headers || {}) },
      ...options,
    };
    if (opts.body && typeof opts.body !== 'string') {
      opts.body = JSON.stringify(opts.body);
    }
    const res = await fetch(path, opts);
    if (res.status === 204) {
      return null;
    }
    const contentType = res.headers.get('content-type') || '';
    const isJson = contentType.includes('application/json');
    const payload = isJson ? await res.json().catch(() => ({})) : await res.text();
    if (!res.ok) {
      const message = payload && payload.error ? payload.error : `Request failed: ${res.status}`;
      const error = new Error(message);
      error.status = res.status;
      error.payload = payload;
      throw error;
    }
    return payload;
  }

  // /api/auth/me를 호출해 세션이 유효하면 사용자 정보를 돌려준다.
  async function me() {
    try {
      return await request('/api/auth/me');
    } catch (err) {
      if (err && err.status === 401) {
        return null;
      }
      throw err;
    }
  }

  // 인증이 필수인 페이지에서 사용: 없으면 401 에러 throw
  async function requireAuth() {
    const user = await me();
    if (user) {
      return user;
    }
    const error = new Error('Unauthorized');
    error.status = 401;
    throw error;
  }

  function redirectToLogin(options = {}) {
    navigateTo('/index.html', options);
  }

  function redirectToLibrary(options = {}) {
    navigateTo('/videos.html', options);
  }

  return {
    request,
    get: (path) => request(path),
    post: (path, body) => request(path, { method: 'POST', body }),
    me,
    requireAuth,
    redirectToLogin,
    redirectToLibrary,
  };
})();

// 초 단위 시간을 "분:초" 혹은 "시:분:초"로 포맷한다.
function formatSeconds(seconds) {
  if (!Number.isFinite(seconds) || seconds <= 0) {
    return '0:00';
  }
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const secs = Math.floor(seconds % 60);
  if (hours > 0) {
    return `${hours}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
  }
  return `${minutes}:${secs.toString().padStart(2, '0')}`;
}

// URLSearchParams 헬퍼
function queryParam(name) {
  const params = new URLSearchParams(window.location.search);
  return params.get(name);
}
