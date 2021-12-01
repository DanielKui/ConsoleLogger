#include "ConsoleLogger.h"

namespace console_logger {
const string_view_t& to_string_view(level_enum l) noexcept
{
    return level_string_views[l];
}
std::tm localtime(const std::time_t& time_tt) noexcept
{
#ifdef _WIN32
    std::tm tm;
    ::localtime_s(&tm, &time_tt);
#else
    std::tm tm;
    ::localtime_r(&time_tt, &tm);
#endif
    return tm;
}

void append_string_view(string_view_t view, memory_buf_t& dest)
{
    auto* buf_ptr = view.data();
    dest.append(buf_ptr, buf_ptr + view.size());
}

void pad2(int n, memory_buf_t& dest)
{
    if (n >= 0 && n < 100) // 0-99
    {
        dest.push_back(static_cast<char>('0' + n / 10));
        dest.push_back(static_cast<char>('0' + n % 10));
    }
    else // unlikely, but just in case, let fmt deal with it
    {
        fmt::format_to(std::back_inserter(dest), fmt::runtime("{:02}"), n);
    }
}

void full_formatter::init_tn()
{
    tn_.init();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    double tsc_ghz = tn_.calibrate();
}

pattern_formatter::pattern_formatter(
    std::string pattern, std::string eol)
    : pattern_(std::move(pattern))
    , eol_(std::move(eol))
    , last_log_secs_(0)
{
    std::memset(&cached_tm_, 0, sizeof(cached_tm_));
}

// use by default full formatter for if pattern is not given
pattern_formatter::pattern_formatter(std::string eol)
    : pattern_("%+")
    , eol_(std::move(eol))
    , last_log_secs_(0)
{
    std::memset(&cached_tm_, 0, sizeof(cached_tm_));
    formatter_ = std::make_unique<full_formatter>();
}

std::unique_ptr<pattern_formatter> pattern_formatter::clone() const
{
    return nullptr;
}

void pattern_formatter::format(const async_msg& msg, memory_buf_t& dest)
{
    uint64_t ns = formatter_->GetTSCNS().tsc2ns(msg.time_tsc);
    auto nanos = std::chrono::nanoseconds(ns);

    log_clock::time_point tp_nanos(std::chrono::duration_cast<log_clock::duration>(nanos));

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(tp_nanos.time_since_epoch());
    if (secs != last_log_secs_)
    {
        cached_tm_ = get_time_(msg);
        last_log_secs_ = secs;
    }

    formatter_->format(msg, cached_tm_, dest);

    // write eol
    dest.append(eol_.data(), eol_.data()+eol_.size());
}

std::tm pattern_formatter::get_time_(const async_msg& msg)
{
    uint64_t ns = formatter_->GetTSCNS().tsc2ns(msg.time_tsc);
    auto nanos = std::chrono::nanoseconds(ns);

    log_clock::time_point tp_nanos(std::chrono::duration_cast<log_clock::duration>(nanos));
    
    return localtime(log_clock::to_time_t(tp_nanos));
}

void async_msg::update_string_views()
{
    logger_name = string_view_t{ buffer.data(), logger_name.size() };
    payload = string_view_t{ buffer.data() + logger_name.size(), payload.size() };
}

async_msg& async_msg::operator=(const async_msg& other)
{
    time_tsc = std::move(other.time_tsc);
    payload = std::move(other.payload);
    logger_name = std::move(other.logger_name);
    level = std::move(other.level);
    color_range_start = std::move(other.color_range_start);
    color_range_end = std::move(other.color_range_end);

    buffer.clear();
    buffer.append(other.buffer.data(), other.buffer.data() + other.buffer.size());
    update_string_views();
    return *this;
}

async_msg& async_msg::operator=(async_msg&& other) noexcept
{
    time_tsc = std::move(other.time_tsc);
    payload = std::move(other.payload);
    logger_name = std::move(other.logger_name);
    level = std::move(other.level);
    color_range_start = std::move(other.color_range_start);
    color_range_end = std::move(other.color_range_end);

    buffer = std::move(other.buffer);
    update_string_views();
    return *this;
}

void async_logger::post_async_msg_(async_msg&& new_msg)
{
    q_.enqueue(std::move(new_msg));    
}

void async_logger::worker_loop_()
{
    while (process_next_msg_()) {}
}

// process next message in the queue
// return true if this thread should still be active (while no terminate msg
// was received)
bool async_logger::process_next_msg_()
{
    async_msg incoming_async_msg;
    bool dequeued = q_.try_dequeue(incoming_async_msg);
    if (!dequeued)
    {
        return true;
    }

    switch (incoming_async_msg.msg_type)
    {
    case async_msg_type::log: {
        backend_sink_it_(incoming_async_msg);
        return true;
    }
    case async_msg_type::flush: {
        backend_flush_();
        return true;
    }
    case async_msg_type::terminate: {
        return false;
    }
    }

    return true;
}
// set formatting for the sinks in this logger.
// each sink will get a separate instance of the formatter object.
void async_logger::set_formatter(std::unique_ptr<pattern_formatter> f)
{
    for (auto it = sinks_.begin(); it != sinks_.end(); ++it)
    {
        if (std::next(it) == sinks_.end())
        {
            // last element - we can be move it.
            (*it)->set_formatter(std::move(f));
            break; // to prevent clang-tidy warning
        }
        else
        {
            (*it)->set_formatter(f->clone());
        }
    }
}

void async_logger::flush_on(level_enum log_level)
{
    flush_level_.store(log_level);
}

// protected methods
void async_logger::log_it_(const async_msg& log_msg)
{
    sink_it_(log_msg);
 }
    
bool async_logger::should_flush_(const async_msg& msg)
{
    auto flush_level = flush_level_.load(std::memory_order_relaxed);
    return (msg.level >= flush_level) && (msg.level != level_enum::off);
}

async_logger::async_logger(
    std::string logger_name, sinks_init_list sinks_list)
    : async_logger(std::move(logger_name), sinks_list.begin(), sinks_list.end())
{}

async_logger::async_logger(
    std::string logger_name, sink_ptr single_sink)
    : async_logger(std::move(logger_name), { std::move(single_sink) })
{}

// send the log message to the thread pool
void async_logger::sink_it_(const async_msg& msg)
{
    async_msg async_m(shared_from_this(), async_msg_type::log, msg);
    post_async_msg_(std::move(async_m));
}

// send flush request to the thread pool
void async_logger::flush_()
{
    post_async_msg_(async_msg(shared_from_this(), async_msg_type::flush));
}

// backend functions - called from the thread pool to do the actual job
void async_logger::backend_sink_it_(const async_msg& msg)
{
    for (auto& sink : sinks_)
    {
        sink->log(msg);
    }

    if (should_flush_(msg))
    {
        backend_flush_();
    }
}

void async_logger::backend_flush_()
{
    for (auto& sink : sinks_)
    {
        sink->flush();
    }
}
}