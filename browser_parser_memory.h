#pragma once

// The browser's UI thread owns parser calls. The scope also releases a parse
// tree if converting it to litehtml nodes throws before normal destruction.
class BrowserParserScope
{
public:
    BrowserParserScope();
    ~BrowserParserScope();
    BrowserParserScope(const BrowserParserScope &) = delete;
    BrowserParserScope &operator=(const BrowserParserScope &) = delete;

private:
    void               *records_ = nullptr;
    BrowserParserScope *previous_ = nullptr;
    friend struct BrowserParserAccess;
};

// Counts parser-owned allocations, including transient tokenizer buffers.
// Useful for checking that navigation returns to a bounded memory baseline.
extern "C" unsigned long long browser_parser_live_allocations();
