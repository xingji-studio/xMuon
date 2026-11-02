#pragma once

#include <litehtml/el_image.h>
#include <string>

// An inline SVG keeps its DOM subtree, but participates in HTML layout and
// painting as one replaced image. Rasterization uses the normal image cache.
class BrowserInlineSvgElement final : public litehtml::el_image
{
public:
    explicit BrowserInlineSvgElement(const litehtml::document::ptr &document);
    void compute_styles(bool recursive = true) override;
    void get_content_size(litehtml::size &size, litehtml::pixel_t max_width) override;
    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position *clip, const std::shared_ptr<litehtml::render_item> &item) override;

    // Returns bounded, self-contained SVG for the existing decoder. No
    // resources are loaded while serializing; unsupported subtrees are omitted.
    std::string source_for_size(unsigned width, unsigned height) const;
};
