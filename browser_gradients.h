#pragma once

#include "browser_image_decode.h"
#include <litehtml/background.h>

struct BrowserGradientRaster
{
    litehtml::position bounds;
    BrowserDecodedImage image;
};

// Inputs use litehtml's world coordinates and normalized color-stop offsets.
// The result is a clipped, pixel-aligned straight-alpha RGBA image; composite it
// with the browser's ordinary source-over image path. Failure leaves out empty.
bool browser_raster_linear_gradient(const litehtml::background_layer &layer, const litehtml::position &clip,
                                    const litehtml::background_layer::linear_gradient &gradient,
                                    BrowserGradientRaster &out);
bool browser_raster_radial_gradient(const litehtml::background_layer &layer, const litehtml::position &clip,
                                    const litehtml::background_layer::radial_gradient &gradient,
                                    BrowserGradientRaster &out);
bool browser_raster_conic_gradient(const litehtml::background_layer &layer, const litehtml::position &clip,
                                   const litehtml::background_layer::conic_gradient &gradient,
                                   BrowserGradientRaster &out);
