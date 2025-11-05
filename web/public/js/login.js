(async function () {
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
})();
