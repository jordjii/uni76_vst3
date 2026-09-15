// Two complete visual treatments share the same controls and native bridge.
// The archive treatment is the new default; the original graphite treatment
// remains available and is not duplicated or rewritten.
const STORAGE_KEY = "uni76-ui-theme";
const ARCHIVE = "archive";
const ORIGINAL = "original";

function readSavedTheme() {
  try {
    // Query override is useful for deterministic visual regression captures;
    // it never persists and normal plugin launches have no such query.
    const preview = new URLSearchParams(window.location.search).get("theme");
    if (preview === ORIGINAL || preview === ARCHIVE) return preview;

    const saved = window.localStorage.getItem(STORAGE_KEY);
    return saved === ORIGINAL || saved === ARCHIVE ? saved : ARCHIVE;
  } catch (_) {
    return ARCHIVE;
  }
}

function saveTheme(theme) {
  try {
    window.localStorage.setItem(STORAGE_KEY, theme);
  } catch (_) {
    // A host may disable storage for its embedded WebView. The theme still
    // works for the current editor session in that case.
  }
}

export function initThemeToggle() {
  const button = document.querySelector('[data-header-control="theme"]');
  if (!button) return;

  const label = button.querySelector(".header__btn-theme-label");

  const apply = (theme) => {
    document.body.dataset.theme = theme;
    button.setAttribute("aria-pressed", String(theme === ARCHIVE));
    button.setAttribute("title", theme === ARCHIVE ? "Switch to original graphite theme" : "Switch to archive theme");
    if (label) label.textContent = theme === ARCHIVE ? "ORIGINAL" : "ARCHIVE";
  };

  apply(readSavedTheme());

  button.addEventListener("click", () => {
    const next = document.body.dataset.theme === ARCHIVE ? ORIGINAL : ARCHIVE;
    apply(next);
    saveTheme(next);
  });
}
