#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Paged viewer for one Readwise Reader article's full body text (SLO-15).
// Mirrors DictionaryDefinitionActivity's two-tier layout: the HTML is laid
// out through the EPUB chapter parser into styled Pages when
// buildDictionaryHtmlPages() (src/util/DictHtmlPages.h) succeeds, otherwise
// converted to plain text (src/util/HtmlToPlainText.h) and word-wrapped once
// into line spans, so no per-line copies of the article are held. Read-only:
// archiving happens on ArticleDetailActivity before this activity opens.
class ArticleReaderActivity final : public Activity {
 public:
  explicit ArticleReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                 std::string html)
      : Activity("ArticleReader", renderer, mappedInput), title(std::move(title)), html(std::move(html)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `html` (post plain-text
  // conversion). Wrapping keeps lines under the screen width, so uint16_t
  // length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  BodyArea bodyArea() const;
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;

  const std::string title;
  // Not const: cleared once styled pages own the text (layoutHtmlPages), or
  // replaced with its plain-text conversion for the wrapped-span path.
  std::string html;
  // Styled path: reader-identical Pages laid out from the HTML. Empty means
  // the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  // Set when neither the styled nor the plain-text path could safely run
  // (see onEnter()): render() shows an error instead of an empty/garbage page.
  bool loadFailed = false;
  ButtonNavigator buttonNavigator;
};
