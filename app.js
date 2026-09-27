// 轻量交互：滚动时给卡片一点入场感，并给数字加千分位（若静态文案被改动）
(function () {
  const cards = document.querySelectorAll(".card");
  if ("IntersectionObserver" in window) {
    const io = new IntersectionObserver(
      (entries) => {
        entries.forEach((e) => {
          if (e.isIntersecting) {
            e.target.style.transition = "opacity .35s ease, transform .35s ease";
            e.target.style.opacity = "1";
            e.target.style.transform = "none";
            io.unobserve(e.target);
          }
        });
      },
      { threshold: 0.15 }
    );
    cards.forEach((c, i) => {
      c.style.opacity = "0";
      c.style.transform = "translateY(8px)";
      c.style.transitionDelay = i * 60 + "ms";
      io.observe(c);
    });
  }

  document.querySelectorAll(".num").forEach((el) => {
    const raw = el.textContent.trim();
    if (/^-?[\d,]+$/.test(raw)) {
      const n = Number(raw.replace(/,/g, ""));
      if (!Number.isNaN(n)) {
        el.textContent = n.toLocaleString("en-US");
      }
    }
  });
})();
