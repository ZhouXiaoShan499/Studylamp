(() => {
  'use strict';

  const header = document.querySelector('[data-site-header]');
  const menu = document.querySelector('#siteMenu');
  const menuToggle = document.querySelector('[data-menu-toggle]');

  /* ------------------------------------------------------------- site year */
  document.querySelectorAll('[data-year]').forEach(node => {
    node.textContent = String(new Date().getFullYear());
  });

  /* --------------------------------------------------------------- header UI */
  if (header) {
    // 浅色区块（首屏 / 浅带）滚到导航下方时，导航切成浅色方案，
    // 否则浅底上会压出一条深色横杠。两处类名不同，都要列。
    // （首页原来的四通道区也是浅底，2026-11-23 该区移除，选择器一并撤掉；
    //   恢复时看 assets/pages-retired.css 的说明。）
    const lightZones = Array.from(document.querySelectorAll('.light-band, .home-hero'));
    const themeMeta = document.querySelector('meta[name="theme-color"]');
    const DARK_UI = '#080907';
    const LIGHT_UI = '#ece9e6';
    let queued = false;
    const syncHeader = () => {
      header.classList.toggle('is-scrolled', window.scrollY > 12);
      if (lightZones.length) {
        // 导航是 position:sticky，占布局位置，首屏区块的 top 恰好等于导航高度，
        // 用 < 会漏掉"页面停在顶部"这一帧，所以留 1px 容差。
        const edge = header.getBoundingClientRect().bottom;
        const overLight = lightZones.some(zone => {
          const rect = zone.getBoundingClientRect();
          return rect.top <= edge + 1 && rect.bottom > 0;
        });
        header.classList.toggle('is-on-light', overLight);
        if (themeMeta) themeMeta.setAttribute('content', overLight ? LIGHT_UI : DARK_UI);
      }
      queued = false;
    };
    syncHeader();
    window.addEventListener('scroll', () => {
      if (queued) return;
      queued = true;
      requestAnimationFrame(syncHeader);
    }, { passive: true });
    window.addEventListener('resize', syncHeader, { passive: true });
  }

  /* -------------------------------------------------------------- mobile nav */
  let lastFocused = null;
  const focusableSelector = 'a[href], button:not([disabled]), [tabindex]:not([tabindex="-1"])';

  function setMenu(open, options = {}) {
    if (!header || !menu || !menuToggle) return;
    if (open) {
      lastFocused = document.activeElement;
      menu.hidden = false;
      header.classList.add('is-open');
      menuToggle.setAttribute('aria-expanded', 'true');
      menuToggle.setAttribute('aria-label', '关闭菜单');
      document.body.classList.add('menu-open');
      if (options.focus !== false) {
        const first = menu.querySelector(focusableSelector);
        if (first) window.setTimeout(() => first.focus(), 30);
      }
    } else {
      header.classList.remove('is-open');
      menuToggle.setAttribute('aria-expanded', 'false');
      menuToggle.setAttribute('aria-label', '打开菜单');
      document.body.classList.remove('menu-open');
      window.setTimeout(() => {
        if (!header.classList.contains('is-open')) menu.hidden = true;
      }, 10);
      if (options.restoreFocus && lastFocused && typeof lastFocused.focus === 'function') {
        lastFocused.focus({ preventScroll: true });
      }
    }
  }

  if (header && menu && menuToggle) {
    menuToggle.addEventListener('click', () => {
      const open = header.classList.contains('is-open');
      setMenu(!open, { restoreFocus: open });
    });

    menu.querySelectorAll('a').forEach(link => {
      link.addEventListener('click', () => setMenu(false));
    });

    document.addEventListener('keydown', event => {
      if (!header.classList.contains('is-open')) return;
      if (event.key === 'Escape') {
        event.preventDefault();
        setMenu(false, { restoreFocus: true });
      }
      if (event.key === 'Tab') {
        // Keep keyboard navigation inside the open menu long enough to make a choice.
        const items = [menuToggle, ...menu.querySelectorAll(focusableSelector)].filter(item => item.offsetParent !== null);
        if (!items.length) return;
        const first = items[0];
        const last = items[items.length - 1];
        if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first.focus();
        }
      }
    });

    document.addEventListener('pointerdown', event => {
      if (!header.classList.contains('is-open')) return;
      if (!header.contains(event.target)) setMenu(false, { restoreFocus: true });
    }, { passive: true });

    window.addEventListener('resize', () => {
      if (window.innerWidth > 900 && header.classList.contains('is-open')) setMenu(false);
    }, { passive: true });
  }

  /* --------------------------------------------------------- reveal on scroll */
  const revealItems = Array.from(document.querySelectorAll('[data-reveal]'));
  const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  if (!revealItems.length) return;

  if (reduceMotion || !('IntersectionObserver' in window)) {
    revealItems.forEach(item => item.classList.add('is-revealed'));
    return;
  }

  const observer = new IntersectionObserver(entries => {
    entries.forEach(entry => {
      if (!entry.isIntersecting) return;
      entry.target.classList.add('is-revealed');
      observer.unobserve(entry.target);
    });
  }, { rootMargin: '0px 0px -8% 0px', threshold: 0.03 });

  revealItems.forEach(item => observer.observe(item));
})();
