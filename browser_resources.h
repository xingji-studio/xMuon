#pragma once

#include "browser_fetch.h"
#include "browser_http_bridge.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

enum BrowserResourceKind : unsigned int
{
    BrowserStylesheet = 1,
    BrowserImage = 2,
    BrowserScript = 4
};

// Main-thread cooperative queue. Jobs survive vector growth, so callback
// contexts and completed results stay valid until the next navigation reset.
class BrowserResourceQueue
{
public:
    struct Job
    {
        std::string url;
        unsigned int kind = 0, priority = 10;
        unsigned long long turn = 0;
        bool finished = false, delivered = false;
        std::unique_ptr<BrowserFetchTask> task;
        BrowserFetchResult response;
    };
    using LocalFetch = bool (*)(const std::string &, BrowserFetchResult &);

    void reset(const BrowserFetchOptions &options, LocalFetch local)
    {
        cancel();
        jobs_.clear();
        options_ = options;
        local_ = local;
        turn_ = 0;
    }

    bool enqueue(const std::string &url, unsigned int kind, unsigned int priority = 10)
    {
        if (url.empty()) return false;
        for (auto &job : jobs_)
            if (job->url == url)
            {
                job->kind |= kind;
                job->priority = std::min(job->priority, priority);
                return true;
            }
        if (jobs_.size() >= 64 && (kind & BrowserScript))
        {
            // Decoded image pixels live in the embedding's image cache. Retire
            // an already released transfer so late script modules can load
            // without increasing the bounded queue or evicting stylesheets.
            const auto retired = std::find_if(jobs_.begin(), jobs_.end(), [](const auto &job) {
                return job->kind == BrowserImage && job->finished && job->delivered && job->response.body.empty();
            });
            if (retired != jobs_.end()) jobs_.erase(retired);
        }
        if (jobs_.size() >= 64) return false;
        std::unique_ptr<Job> job(new Job);
        job->url = url;
        job->kind = kind;
        job->priority = priority;
        jobs_.push_back(std::move(job));
        return true;
    }

    const Job *find(const std::string &url) const
    {
        for (const auto &job : jobs_) if (job->url == url) return job.get();
        return nullptr;
    }

    bool provide(const std::string &url, const BrowserFetchResult &response, unsigned int kind,
                 unsigned int priority = 10)
    {
        if (!enqueue(url, kind, priority)) return false;
        for (auto &job : jobs_)
            if (job->url == url && !job->task &&
                (!job->finished || ((kind & BrowserImage) && job->delivered && job->response.ok &&
                                    job->response.status_code >= 200 && job->response.status_code < 300 &&
                                    job->response.body.empty() && !response.body.empty())))
            {
                job->response = response;
                job->finished = true;
                job->delivered = false;
                break;
            }
        return true;
    }

    void refetch_image(const std::string &url)
    {
        for (auto &job : jobs_)
            if (job->url == url && job->finished && job->delivered && job->response.ok &&
                job->response.status_code >= 200 && job->response.status_code < 300 &&
                job->response.body.empty() && (job->kind & BrowserImage))
            {
                job->response = BrowserFetchResult();
                job->finished = job->delivered = false;
                return;
            }
    }

    void release_body(const std::string &url)
    {
        for (auto &job : jobs_)
            if (job->url == url && job->finished) std::string().swap(job->response.body);
    }

    bool pending(unsigned int kind = ~0U) const
    {
        for (const auto &job : jobs_)
            if ((!job->finished || !job->delivered) && (job->kind & kind)) return true;
        return false;
    }

    unsigned int active() const
    {
        unsigned int count = 0;
        for (const auto &job : jobs_) if (job->task && job->task->active()) ++count;
        return count;
    }

    void cancel()
    {
        for (auto &job : jobs_)
        {
            if (job->task) job->task->cancel();
            if (!job->finished)
            {
                job->response.cancelled = true;
                job->response.error_code = -125;
                job->finished = true;
            }
            job->delivered = true;
        }
    }

    std::vector<const Job *> poll(unsigned int external_active = 0)
    {
        std::vector<Job *> order;
        for (auto &job : jobs_) if (!job->finished) order.push_back(job.get());
        std::stable_sort(order.begin(), order.end(), [](const Job *a, const Job *b) {
            return a->priority != b->priority ? a->priority < b->priority : a->turn < b->turn;
        });
        auto count = active() + external_active;
        const auto started = browser_http_now_ms();
        for (auto *job : order)
        {
            if (job->finished) continue; // A prior callback may cancel the whole queue.
            if (!job->task)
            {
                if (local_ && local_(job->url, job->response)) job->finished = true;
                else
                {
                    if (count >= 4) continue;
                    // While critical CSS is pending, encoded image results
                    // wait for the first paint before decoding. Apply pressure
                    // instead of accumulating an entire page of large bodies.
                    std::size_t retained = 0;
                    for (const auto &entry : jobs_)
                        if (entry->finished) retained += entry->response.body.size();
                    if ((job->kind & BrowserImage) && !(job->kind & BrowserStylesheet) &&
                        retained >= 16 * 1024 * 1024) continue;
                    auto options = options_;
                    if (job->kind & BrowserStylesheet) options.max_response_bytes = 256 * 1024;
                    if (job->kind & BrowserScript) options.max_response_bytes = 2 * 1024 * 1024;
                    job->task.reset(new BrowserFetchTask(job->url, options));
                }
            }
            if (!job->finished)
            {
                job->turn = ++turn_;
                const bool was_active = job->task->active();
                if (job->task->poll(was_active || count < 4))
                {
                    job->response = job->task->take_result();
                    job->finished = true;
                }
                const bool is_active = job->task->active();
                if (was_active && !is_active) --count;
                if (!was_active && is_active) ++count;
                if (job->finished) job->task.reset();
            }
            const auto now = browser_http_now_ms();
            if (now == ~0ULL || now < started || now - started >= 12) break;
        }
        std::vector<const Job *> completed;
        for (auto &job : jobs_)
            if (job->finished && !job->delivered)
            {
                job->delivered = true;
                completed.push_back(job.get());
            }
        return completed;
    }

private:
    BrowserFetchOptions options_;
    LocalFetch local_ = nullptr;
    unsigned long long turn_ = 0;
    std::vector<std::unique_ptr<Job>> jobs_;
};
