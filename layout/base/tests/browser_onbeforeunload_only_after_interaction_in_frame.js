function pageScript() {
  window.addEventListener(
    "beforeunload",
    function(event) {
      var str = "Some text that causes the beforeunload dialog to be shown";
      event.returnValue = str;
      return str;
    },
    true
  );
}

SpecialPowers.pushPrefEnv({
  set: [
    ["dom.require_user_interaction_for_beforeunload", true],
    ["security.allow_eval_with_system_principal", true],
  ],
});

const FRAME_URL =
  "data:text/html," + encodeURIComponent("<body>Just a frame</body>");

const PAGE_URL =
  "data:text/html," +
  encodeURIComponent(
    "<iframe src='" +
      FRAME_URL +
      "'></iframe><script>(" +
      pageScript.toSource() +
      ")();</script>"
  );

add_task(async function doClick() {
  // The onbeforeunload dialog should appear.
  let dialogShown = false;
  function onDialogShown(node) {
    dialogShown = true;
    let dismissButton = node.querySelector(".tabmodalprompt-button0");
    dismissButton.click();
  }
  let obsName = "tabmodal-dialog-loaded";
  Services.obs.addObserver(onDialogShown, obsName);
  await openPage(true);
  Services.obs.removeObserver(onDialogShown, obsName);
  Assert.ok(dialogShown, "Should have shown dialog.");
});

add_task(async function noClick() {
  // The onbeforeunload dialog should NOT appear.
  await openPage(false);
  info("If we time out here, then the dialog was shown...");
});

async function openPage(shouldClick) {
  // Open about:blank in a new tab.
  await BrowserTestUtils.withNewTab(
    { gBrowser, url: "about:blank" },
    async function(browser) {
      // Load the page.
      BrowserTestUtils.loadURI(browser, PAGE_URL);
      await BrowserTestUtils.browserLoaded(browser);

      let frameBC = browser.browsingContext.children[0];
      if (shouldClick) {
        await BrowserTestUtils.synthesizeMouse("body", 2, 2, {}, frameBC);
      }
      let hasInteractedWith = await SpecialPowers.spawn(
        frameBC,
        [],
        function() {
          return [
            content.document.userHasInteracted,
            content.document.userHasInteracted,
          ];
        }
      );
      is(
        shouldClick,
        hasInteractedWith[0],
        "Click should update parent interactivity state"
      );
      is(
        shouldClick,
        hasInteractedWith[1],
        "Click should update frame interactivity state"
      );
      // And then navigate away.
      BrowserTestUtils.loadURI(browser, "http://example.com/");
      await BrowserTestUtils.browserLoaded(browser);
    }
  );
}
