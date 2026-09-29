#include "ArticleDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "ArticleOfflineCache.h"
#include "ArticleReaderActivity.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ArticleDetailActivity::onEnter() {
  Activity::onEnter();
  // Reconstructed post-restart specifically to keep reading this article
  // (see this class's header comment) — skip the summary and fetch now.
  if (autoRead) state = State::LOADING;
  requestUpdateAndWait();
}

void ArticleDetailActivity::openReader() {
  requestUpdateAndWait();  // render "Loading article..." before the blocking call
  std::string html;
  const auto err = ReadwiseClient::fetchHtmlContent(article.id, html);
  if (err != ReadwiseClient::OK) {
    readFailed = true;
    state = State::VIEWING;
    requestUpdate();
    return;
  }
  readFailed = false;
  state = State::VIEWING;
  startActivityForResult(std::make_unique<ArticleReaderActivity>(renderer, mappedInput, article.title, std::move(html)),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void ArticleDetailActivity::loop() {
  if (state == State::ARCHIVING) {
    requestUpdateAndWait();  // render "Archiving..." before the blocking call
    if (ReadwiseClient::archive(article.id) == ReadwiseClient::OK) {
      setResult(ArticleDetailResult{true});
      finish();
      return;
    }
    archiveFailed = true;
    state = State::VIEWING;
    requestUpdate();
    return;
  }
  if (state == State::LOADING) {
    openReader();
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Already downloaded (ArticleModuleActivity::downloadCachedArticleText())
    // — no new WiFi/TLS session needed, so open directly instead of paying
    // for the restart-based heap defrag below.
    std::string html;
    if (ArticleOfflineCache::loadText(article.id, html)) {
      startActivityForResult(
          std::make_unique<ArticleReaderActivity>(renderer, mappedInput, article.title, std::move(html)),
          [this](const ActivityResult&) { requestUpdate(); });
      return;
    }
    // Reboots (see this class's header comment); this activity is torn down
    // here, not resumed — the far side is a fresh ArticleModuleActivity with
    // autoRead=true, not a continuation of this loop().
    silentRestartToArticleRead(article.id);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    archiveFailed = false;
    state = State::ARCHIVING;
    requestUpdate();
  }
}

void ArticleDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 article.title.empty() ? tr(STR_ARTICLE_MODULE_TITLE) : article.title.c_str());

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (!article.author.empty()) {
    renderer.drawText(UI_10_FONT_ID, x, y, article.author.c_str());
    y += lineHeight + metrics.verticalSpacing;
  }

  if (article.wordCount > 0) {
    char wordCountBuf[32];
    snprintf(wordCountBuf, sizeof(wordCountBuf), tr(STR_ARTICLE_WORD_COUNT), article.wordCount);
    renderer.drawText(UI_10_FONT_ID, x, y, wordCountBuf);
    y += lineHeight + metrics.verticalSpacing;
  }

  y += metrics.verticalSpacing;

  if (state == State::ARCHIVING) {
    renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_ARTICLE_ARCHIVING));
  } else if (state == State::LOADING) {
    renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_ARTICLE_LOADING_ARTICLE));
  } else {
    if (archiveFailed) {
      renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_ARTICLE_ARCHIVE_FAILED));
      y += lineHeight + metrics.verticalSpacing * 2;
    }
    if (readFailed) {
      renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_ARTICLE_READ_FAILED));
      y += lineHeight + metrics.verticalSpacing * 2;
    }
    const char* summary = article.summary.empty() ? tr(STR_ARTICLE_NO_SUMMARY) : article.summary.c_str();
    auto summaryLines = renderer.wrappedText(UI_10_FONT_ID, summary, contentWidth, 12);
    for (const auto& line : summaryLines) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += lineHeight;
    }
  }

  const bool busy = state != State::VIEWING;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), busy ? "" : tr(STR_ARTICLE_READ), "", busy ? "" : tr(STR_ARTICLE_ARCHIVE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
