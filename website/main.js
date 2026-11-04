(() => {
  const root = document.documentElement;
  const burger = document.querySelector(".nav__burger");
  const menu = document.querySelector("#mobile-menu");
  const themeToggle = document.querySelector(".theme-toggle");

  if (themeToggle) {
    const syncPressed = () =>
      themeToggle.setAttribute("aria-pressed", String(root.dataset.theme === "dark"));
    syncPressed();
    themeToggle.addEventListener("click", () => {
      const next = root.dataset.theme === "dark" ? "light" : "dark";
      root.dataset.theme = next;
      try { localStorage.setItem("theme", next); } catch {}
      syncPressed();
    });
  }

  if (!burger || !menu) return;

  const setOpen = (open) => {
    burger.classList.toggle("is-open", open);
    burger.setAttribute("aria-expanded", String(open));
    burger.setAttribute("aria-label", open ? "Close menu" : "Open menu");
    menu.hidden = !open;
  };

  burger.addEventListener("click", () => setOpen(menu.hidden));
  menu.addEventListener("click", (event) => {
    if (event.target.closest("a")) setOpen(false);
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !menu.hidden) {
      setOpen(false);
      burger.focus();
    }
  });
})();