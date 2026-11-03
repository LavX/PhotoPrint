(() => {
  const burger = document.querySelector(".nav__burger");
  const menu = document.getElementById("mobile-menu");
  if (!burger || !menu) return;

  const desktop = window.matchMedia("(min-width: 821px)");

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

  desktop.addEventListener("change", (event) => {
    if (event.matches) setOpen(false);
  });
})();
