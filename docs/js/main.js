// Gamma Code site — vanilla JS only: demo video playback policy, copy buttons,
// method tabs, sticky-header state, scroll reveal.
// Everything degrades fine if this file fails to load (static content shown).

(function () {
  "use strict";

  var reducedMotion = window.matchMedia &&
    window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ─────────────── demo video ─────────────── */

  // The hero shows a real capture of the app (Gamma.mp4), autoplaying muted
  // and looping like a product demo. With prefers-reduced-motion we must not
  // move things on our own: pause it and hand the user the controls instead.
  function initDemoVideo() {
    var video = document.querySelector("video.term-video");
    if (!video) return;
    if (reducedMotion) {
      video.removeAttribute("autoplay");
      video.pause();
      video.controls = true;
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

  /* ─────────────── sticky header state ─────────────── */

  // Adds .scrolled to the header once the page has moved off the top, so
  // the bottom border appears only when content is scrolling under it.
  function initHeader() {
    var head = document.querySelector(".site-head");
    if (!head) return;
    function update() {
      head.classList.toggle("scrolled", window.pageYOffset > 4);
    }
    update();
    window.addEventListener("scroll", update, { passive: true });
  }

  /* ─────────────── scroll reveal ─────────────── */

  // Cards and section heads fade in as they enter the viewport. The .reveal
  // class is added here (not in markup) so content is fully visible if JS
  // never runs. Reduced motion shows everything immediately.
  function initReveal() {
    var targets = document.querySelectorAll(
      ".card, .banner, .release-strip, .tiers, .tablewrap, .diagram, .reqs, .smartscreen"
    );
    if (!targets.length) return;

    if (reducedMotion || !("IntersectionObserver" in window)) {
      return; // no .reveal added => content stays visible
    }

    for (var i = 0; i < targets.length; i++) {
      targets[i].classList.add("reveal");
    }

    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (!entry.isIntersecting) return;
        entry.target.classList.add("in");
        observer.unobserve(entry.target);
      });
    }, { threshold: 0.12, rootMargin: "0px 0px -40px 0px" });

    for (var j = 0; j < targets.length; j++) {
      observer.observe(targets[j]);
    }
  }

  /* ─────────────── boot ─────────────── */

  function init() {
    initDemoVideo();
    initCopyButtons();
    initTabs();
    initHeader();
    initReveal();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
