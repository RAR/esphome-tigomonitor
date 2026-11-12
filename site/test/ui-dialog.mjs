// site/test/ui-dialog.mjs
// The app asks before a change in a dialog of its own (#ui-dialog, app.html's
// uiAsk), not the browser's confirm(); these answer it as a user would.

// Waits for the question, answers it, and returns its title.
export async function answer(page, yes) {
  await page.waitForSelector('#ui-dialog[open]');
  const title = await page.textContent('#ui-dialog h3');
  await page.click(`#ui-dialog [data-answer="${yes ? 'ok' : 'cancel'}"]`);
  await page.waitForSelector('#ui-dialog:not([open])', { state: 'attached' });
  return title;
}

// Answers every question from now on, this page and the next it loads.
export async function answerAll(page, yes) {
  const hook = (yes) => {
    const watch = () => {
      const d = document.getElementById('ui-dialog');
      if (!d) return setTimeout(watch, 20);
      new MutationObserver(() => {
        if (d.open) setTimeout(() => d.querySelector(`[data-answer="${yes ? 'ok' : 'cancel'}"]`).click(), 0);
      }).observe(d, { attributes: true, attributeFilter: ['open'] });
    };
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', watch);
    else watch();
  };
  await page.addInitScript(hook, yes);
  if (page.url() !== 'about:blank') await page.evaluate(hook, yes);
}
