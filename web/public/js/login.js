// 로그인 및 회원가입 폼을 제어하는 스크립트
(async function () {
  // 페이지가 로드되면 먼저 기존 세션이 있는지 확인해 바로 리디렉션한다.
  try {
    const user = await api.me();
    if (user) {
      api.redirectToLibrary({ replace: true });
      return;
    }
  } catch (err) {
    console.warn('Session check failed', err);
  }

  const form = document.getElementById('login-form');
  const errorBox = document.getElementById('login-error');
  const registerForm = document.getElementById('register-form');
  const registerError = document.getElementById('register-error');
  const registerSuccess = document.getElementById('register-success');

  // 로그인 버튼: API 호출 후 성공 시 라이브러리로 이동
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    errorBox.style.display = 'none';
    const formData = new FormData(form);
    const username = formData.get('username').trim();
    const password = formData.get('password');
    if (!username || !password) {
      showError('Please enter username and password.');
      return;
    }
    try {
      await api.post('/api/auth/login', { username, password });
      api.redirectToLibrary({ replace: true });
    } catch (err) {
      showError(err.message || 'Unable to sign in.');
    }
  });

  function showError(message) {
    errorBox.textContent = message;
    errorBox.style.display = 'block';
  }

  if (registerForm) {
    // 회원가입 제출 시 간단한 유효성 검사 후 POST /register
    registerForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      hideRegisterMessages();
      const formData = new FormData(registerForm);
      const username = (formData.get('newUsername') || '').trim();
      const password = formData.get('newPassword');
      const confirmPassword = formData.get('confirmPassword');
      if (!username || !password || !confirmPassword) {
        showRegisterError('Please fill out every field.');
        return;
      }
      if (password !== confirmPassword) {
        showRegisterError('Passwords must match.');
        return;
      }
      try {
        await api.post('/api/auth/register', { username, password, confirmPassword });
        if (registerSuccess) {
          registerSuccess.textContent = 'Account created! Redirecting...';
          registerSuccess.style.display = 'block';
        }
        api.redirectToLibrary({ replace: true });
      } catch (err) {
        showRegisterError(err.message || 'Unable to create account.');
      }
    });
  }

  function hideRegisterMessages() {
    if (registerError) {
      registerError.style.display = 'none';
    }
    if (registerSuccess) {
      registerSuccess.style.display = 'none';
    }
  }

  function showRegisterError(message) {
    if (!registerError) return;
    registerError.textContent = message;
    registerError.style.display = 'block';
  }
})();
