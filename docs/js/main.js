// Gamma Code site — vanilla JS only: typewriter, copy buttons, method tabs.
// Everything degrades fine if this file fails to load (static content shown).

(function () {
  "use strict";

  var reducedMotion = window.matchMedia &&
    window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ─────────────── typewriter ─────────────── */

  // Types the one hero prompt line. With reduced motion the full text is
  // shown immediately, no animation.
  function tw() {
    var el = document.getElementById("typed");
    if (!el) return;
    var text = el.getAttribute("data-text") || "";
    if (reducedMotion) {
      el.textContent = text;
      revealExchanges();
      return;
    }
    var i = 0;
    var speed = 34; // ms per char
    (function step() {
      if (i <= text.length) {
        el.textContent = text.slice(0, i);
        i += 1;
        window.setTimeout(step, speed);
      } else {
        revealExchanges();
      }
    })();
  }

  // The exchange lines under the typed prompt stay invisible until the
  // prompt is fully typed (or immediately, with reduced motion).
  function revealExchanges() {
    var lines = document.querySelectorAll(".exchange");
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i];
      var delay = reducedMotion ? 0 : 140 * i;
      window.setTimeout(function (node) {
        node.classList.add("on");
      }.bind(null, line), delay);
    }
  }

  /* ─────────────── copy buttons ─────────────── */

  function initCopyButtons() {
    var buttons = document.querySelectorAll(".copybtn");
    for (var i = 0; i < buttons.length; i++) {
      buttons[i].addEventListener("click", onClick);
    }

    function onClick(event) {
      var button = event.currentTarget;
      var text = button.getAttribute("data-copy") || "";
      var note = button.querySelector(".copied-note");
      var label = button.querySelector("span:not(.copied-note)") || button;

      function announce(ok) {
        if (note) note.textContent = ok ? "copied" : "";
        button.classList.add("copied");
        window.setTimeout(function () {
          button.classList.remove("copied");
          if (note) note.textContent = "";
          label.textContent = "copy";
        }, 1500);
      }

      function fallbackCopy(value) {
        // ExecCommand fallback for non-secure contexts (file:// pages).
        var ta = document.createElement("textarea");
        ta.value = value;
        ta.setAttribute("readonly", "");
        ta.style.position = "fixed";
        ta.style.left = "-9999px";
        document.body.appendChild(ta);
        ta.select();
        var ok = false;
        try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
        document.body.removeChild(ta);
        label.textContent = ok ? "copied" : "copy";
        announce(ok);
      }

      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () {
          label.textContent = "copied";
          announce(true);
        }, function () {
          fallbackCopy(text);
        });
      } else {
        fallbackCopy(text);
      }
    }
  }

  /* ─────────────── install method tabs ─────────────── */

  function initTabs() {
    var tablist = document.querySelector('[role="tablist"]');
    if (!tablist) return;
    var tabs = Array.prototype.slice.call(tablist.querySelectorAll('[role="tab"]'));

    function activate(tab, focus) {
      tabs.forEach(function (t) {
        var selected = t === tab;
        t.setAttribute("aria-selected", selected ? "true" : "false");
        t.tabIndex = selected ? 0 : -1;
        var panel = document.getElementById(t.getAttribute("aria-controls"));
        if (panel) panel.hidden = !selected;
      });
      if (focus) tab.focus();
    }

    tabs.forEach(function (tab, index) {
      tab.addEventListener("click", function () { activate(tab, false); });

      tab.addEventListener("keydown", function (event) {
        var next = null;
        switch (event.key) {
          case "ArrowRight":
          case "ArrowDown":
            next = tabs[(index + 1) % tabs.length];
            break;
          case "ArrowLeft":
          case "ArrowUp":
            next = tabs[(index - 1 + tabs.length) % tabs.length];
            break;
          case "Home":
            next = tabs[0];
            break;
          case "End":
            next = tabs[tabs.length - 1];
            break;
          default:
            return;
        }
        event.preventDefault();
        activate(next, true);
      });
    });
  }

  /* ─────────────── boot ─────────────── */

  function init() {
    tw();
    initCopyButtons();
    initTabs();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
