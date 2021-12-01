#pragma once
#include <atomic> 
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

#include "tscns.h"
#include "moodyCamel/concurrentqueue.h"

#include "fmt/core.h"
#include "fmt/format.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>//isatty() header
#endif

namespace console_logger {
#    ifdef _WIN32
#        define CONSOLE_LOGGER_EOL "\r\n"
#    else
#        define CONSOLE_LOGGER_EOL "\n"
#    endif
constexpr static const char* default_eol = CONSOLE_LOGGER_EOL;

using string_view_t = fmt::basic_string_view<char>;
using level_t = std::atomic<int>;
using memory_buf_t = fmt::basic_memory_buffer<char, 250>;
using log_clock = std::chrono::system_clock;

enum level_enum
{
    info = 0,
    err = 1,
    critical = 2,
    off = 3,
    n_levels
};

#define CONSOLE_LOGGER_LEVEL_NAME_INFO string_view_t("info", 4)
#define CONSOLE_LOGGER_LEVEL_NAME_ERROR string_view_t("error", 5)
#define CONSOLE_LOGGER_LEVEL_NAME_CRITICAL string_view_t("critical", 8)
#define CONSOLE_LOGGER_LEVEL_NAME_OFF string_view_t("off", 3)

#define CONSOLE_LOGGER_LEVEL_NAMES                                                                                                             \
{                                                             \
CONSOLE_LOGGER_LEVEL_NAME_INFO, CONSOLE_LOGGER_LEVEL_NAME_ERROR,  \
    CONSOLE_LOGGER_LEVEL_NAME_CRITICAL, CONSOLE_LOGGER_LEVEL_NAME_OFF                                                                          \
}
static string_view_t level_string_views[] CONSOLE_LOGGER_LEVEL_NAMES;
const string_view_t& to_string_view(level_enum l) noexcept;


std::tm localtime(const std::time_t& time_tt) noexcept;
void append_string_view(string_view_t view, memory_buf_t& dest);

template<typename T>
void append_int(T n, memory_buf_t& dest)
{
    fmt::format_int i(n);
    dest.append(i.data(), i.data() + i.size());
}

void pad2(int n, memory_buf_t& dest);

template<typename T>
inline void pad3(T n, memory_buf_t& dest)
{
    static_assert(std::is_unsigned<T>::value, "pad3 must get unsigned T");
    if (n < 1000)
    {
        dest.push_back(static_cast<char>(n / 100 + '0'));
        n = n % 100;
        dest.push_back(static_cast<char>((n / 10) + '0'));
        dest.push_back(static_cast<char>((n % 10) + '0'));
    }
    else
    {
        append_int(n, dest);
    }
}

// return fraction of a second of the given time_point.
// e.g.
// fraction<std::milliseconds>(tp) -> will return the millis part of the second
template<typename ToDuration>
inline ToDuration time_fraction(log_clock::time_point tp)
{
    auto duration = tp.time_since_epoch();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
    return std::chrono::duration_cast<ToDuration>(duration) - std::chrono::duration_cast<ToDuration>(secs);
}

struct console_mutex
{
    using mutex_t = std::mutex;
    static mutex_t& mutex()
    {
        static mutex_t s_mutex;
        return s_mutex;
    }
};

class async_logger;
using async_logger_ptr = std::shared_ptr<async_logger>;

enum class async_msg_type
{
    log,
    flush,
    terminate
};

// Async msg to move to/from the queue
// Movable only. should never be copied
struct async_msg 
{
    async_msg_type msg_type{ async_msg_type::log };
    async_logger_ptr worker_ptr;

    async_msg() = default;
    ~async_msg() = default;

    // should only be moved in or out of the queue..
    async_msg(const async_msg&) = delete;
    async_msg(async_msg&&) = default;

    // construct from log_msg with given type
    async_msg(async_logger_ptr&& worker, async_msg_type the_type, const async_msg& m)
         : msg_type{ m.msg_type }
        , logger_name(m.logger_name)
        , level(m.level)
        , color_range_start(m.color_range_start)
        , color_range_end(m.color_range_end)
        , time_tsc(m.time_tsc)
        , payload(m.payload)
        , worker_ptr{ std::move(worker) }
    {
        buffer.append(m.logger_name.begin(), m.logger_name.end());
        buffer.append(m.payload.begin(), m.payload.end());
        update_string_views();
    }

    async_msg(async_logger_ptr&& worker, async_msg_type the_type)
         : msg_type{ the_type }
        , worker_ptr{ std::move(worker) }
    {
        buffer.append(logger_name.begin(), logger_name.end());
        buffer.append(payload.begin(), payload.end());
        update_string_views();
    }

    async_msg(string_view_t loggername, level_enum lvl, string_view_t msg)
    : logger_name(loggername)
    , level(lvl)
    , time_tsc(TSCNS::rdtsc())
    , payload(msg)
    {
        buffer.append(logger_name.begin(), logger_name.end());
        buffer.append(payload.begin(), payload.end());
        update_string_views();
    }

    async_msg& operator=(const async_msg& other);
    async_msg& operator=(async_msg&& other) noexcept;

    explicit async_msg(async_msg_type the_type)
        : async_msg{ nullptr, the_type }
    {}

    memory_buf_t buffer;
    void update_string_views();

    string_view_t logger_name;
    level_enum level{ level_enum::off };
    uint64_t time_tsc;

    // wrapping the formatted text with color (updated by pattern_formatter).
    mutable size_t color_range_start{ 0 };
    mutable size_t color_range_end{ 0 };

    string_view_t payload;
};

// Full info formatter
// pattern: [%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%s:%#] %v
class full_formatter 
{
public:
    explicit full_formatter()
    {
        init_tn();
    }

    void format(const async_msg& msg, const std::tm& tm_time, memory_buf_t& dest) 
    {
        using std::chrono::duration_cast;
        using std::chrono::milliseconds;
        using std::chrono::seconds;

        // cache the date/time part for the next ns.
        uint64_t ns = tn_.tsc2ns(msg.time_tsc);
        auto nanos = std::chrono::nanoseconds(ns);
        log_clock::time_point tp_nanos1(std::chrono::duration_cast<log_clock::duration>(nanos));

        auto duration = tp_nanos1.time_since_epoch();
        auto secs = duration_cast<seconds>(duration);

        if (cache_timestamp_ != secs || cached_datetime_.size() == 0)
        {
            cached_datetime_.clear();
            cached_datetime_.push_back('[');
            append_int(tm_time.tm_year + 1900, cached_datetime_);
            cached_datetime_.push_back('-');

            pad2(tm_time.tm_mon + 1, cached_datetime_);
            cached_datetime_.push_back('-');

            pad2(tm_time.tm_mday, cached_datetime_);
            cached_datetime_.push_back(' ');

            pad2(tm_time.tm_hour, cached_datetime_);
            cached_datetime_.push_back(':');

            pad2(tm_time.tm_min, cached_datetime_);
            cached_datetime_.push_back(':');

            pad2(tm_time.tm_sec, cached_datetime_);
            cached_datetime_.push_back('.');

            cache_timestamp_ = secs;
        }
        dest.append(cached_datetime_.begin(), cached_datetime_.end());

        nanos = std::chrono::nanoseconds(ns);
        std::chrono::duration<log_clock::duration::rep, log_clock::duration::period> duration_nanos(nanos.count());
        log_clock::time_point tp_nanos(duration_nanos);

        auto millis = time_fraction<std::chrono::milliseconds>(tp_nanos);
        pad3(static_cast<uint32_t>(millis.count()), dest);
        dest.push_back(']');
        dest.push_back(' ');

        // append logger name if exists
        if (msg.logger_name.size() > 0)
        {
            dest.push_back('[');
            append_string_view(msg.logger_name, dest);
            dest.push_back(']');
            dest.push_back(' ');
        }

        dest.push_back('[');
        // wrap the level name with color
        msg.color_range_start = dest.size();
        append_string_view(to_string_view(msg.level), dest);
        msg.color_range_end = dest.size();
        dest.push_back(']');
        dest.push_back(' ');

        append_string_view(msg.payload, dest);
    }

    const TSCNS& GetTSCNS()
    {
        return tn_;
    }

private:
    void init_tn();

    TSCNS tn_;
    std::chrono::seconds cache_timestamp_{ 0 };
    memory_buf_t cached_datetime_;
};

class pattern_formatter
{
public:
    explicit pattern_formatter(std::string pattern, std::string eol = default_eol);
    // use default pattern is not given
    explicit pattern_formatter(std::string eol = default_eol);
    
    std::unique_ptr<pattern_formatter> clone() const;
    void format(const async_msg& msg, memory_buf_t& dest) ;

private:
    std::string pattern_;
    std::string eol_;
    std::tm cached_tm_;
    std::chrono::seconds last_log_secs_;
    std::unique_ptr<full_formatter> formatter_;
    std::tm get_time_(const async_msg& msg);
};

/*
* Windows color console sink. Uses WriteConsoleA to write to the console with colors
*/
#ifdef _WIN32
template<typename ConsoleMutex>
class wincolor_sink 
{
public:
    explicit wincolor_sink();
    ~wincolor_sink();

    // change the color for the given level
    void set_color(level_enum level, std::uint16_t color);
    void log(const async_msg& msg);
    void flush();
    void set_formatter(std::unique_ptr<pattern_formatter> sink_formatter);
    void set_color_mode();

protected:
    using mutex_t = typename ConsoleMutex::mutex_t;
    void* out_handle_;
    std::mutex& mutex_;
    bool should_do_colors_;
    std::unique_ptr<pattern_formatter> formatter_;
    std::array<std::uint16_t, level_enum::n_levels> colors_;

    // set foreground color and return the orig console attributes (for resetting later)
    std::uint16_t set_foreground_color_(std::uint16_t attribs);

    // print a range of formatted message to console
    void print_range_(const memory_buf_t& formatted, size_t start, size_t end);
    void set_color_mode_impl();
};

template<typename ConsoleMutex>
wincolor_sink<ConsoleMutex>::wincolor_sink()
    : out_handle_(::GetStdHandle(STD_OUTPUT_HANDLE))
    , mutex_(ConsoleMutex::mutex())
    , formatter_(std::make_unique<pattern_formatter>())
{
    set_color_mode_impl();
    // set level colors
    colors_[level_enum::info] = FOREGROUND_GREEN;                                         // green
    colors_[level_enum::err] = FOREGROUND_RED | FOREGROUND_INTENSITY;                     // intense red
    colors_[level_enum::critical] =
        BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; // intense white on red background
    colors_[level_enum::off] = 0;
}

template<typename ConsoleMutex>
wincolor_sink<ConsoleMutex>::~wincolor_sink()
{
    this->flush();
}

// change the color for the given level
template<typename ConsoleMutex>
void wincolor_sink<ConsoleMutex>::set_color(level_enum level, std::uint16_t color)
{
    std::lock_guard<mutex_t> lock(mutex_);
    colors_[level] = color;
}

template<typename ConsoleMutex>
void wincolor_sink<ConsoleMutex>::log(const async_msg& msg)
{
    if (out_handle_ == nullptr || out_handle_ == INVALID_HANDLE_VALUE)
    {
        return;
    }
        
    msg.color_range_start = 0;
    msg.color_range_end = 0;
    memory_buf_t formatted;

    std::lock_guard<mutex_t> lock(mutex_);
    formatter_->format(msg, formatted);
    if (should_do_colors_ && msg.color_range_end > msg.color_range_start)
    {
        // before color range
        print_range_(formatted, 0, msg.color_range_start);
        // in color range
        auto orig_attribs = static_cast<WORD>(set_foreground_color_(colors_[msg.level]));
        print_range_(formatted, msg.color_range_start, msg.color_range_end);
        // reset to orig colors
        ::SetConsoleTextAttribute(static_cast<HANDLE>(out_handle_), orig_attribs);
        print_range_(formatted, msg.color_range_end, formatted.size());
    }
}

template<typename ConsoleMutex>
void wincolor_sink<ConsoleMutex>::flush()
{
    // windows console always flushed?
}

template<typename ConsoleMutex>
void wincolor_sink<ConsoleMutex>::set_formatter(std::unique_ptr<pattern_formatter> sink_formatter)
{
    std::lock_guard<mutex_t> lock(mutex_);
    formatter_ = std::move(sink_formatter);
}

template<typename ConsoleMutex>
void wincolor_sink<ConsoleMutex>::set_color_mode()
{
    std::lock_guard<mutex_t> lock(mutex_);
    set_color_mode_impl();
}

template<typename ConsoleMutex>
void wincolor_sink<ConsoleMutex>::set_color_mode_impl()
{
    // should do colors only if out_handle_  points to actual console.
    DWORD console_mode;
    should_do_colors_ = ::GetConsoleMode(static_cast<HANDLE>(out_handle_), &console_mode) != 0;
}

// set foreground color and return the orig console attributes (for resetting later)
template<typename ConsoleMutex>
std::uint16_t wincolor_sink<ConsoleMutex>::set_foreground_color_(std::uint16_t attribs)
{
    CONSOLE_SCREEN_BUFFER_INFO orig_buffer_info;
    if (!::GetConsoleScreenBufferInfo(static_cast<HANDLE>(out_handle_), &orig_buffer_info))
    {
        // just return white if failed getting console info
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    // change only the foreground bits (lowest 4 bits)
    auto new_attribs = static_cast<WORD>(attribs) | (orig_buffer_info.wAttributes & 0xfff0);
    auto ignored = ::SetConsoleTextAttribute(static_cast<HANDLE>(out_handle_), static_cast<WORD>(new_attribs));
    (void)(ignored);
    return static_cast<std::uint16_t>(orig_buffer_info.wAttributes); // return orig attribs
}

// print a range of formatted message to console
template<typename ConsoleMutex>
void wincolor_sink<ConsoleMutex>::print_range_(const memory_buf_t& formatted, size_t start, size_t end)
{
    if (end > start)
    {
        auto size = static_cast<DWORD>(end - start);
        auto ignored = ::WriteConsoleA(static_cast<HANDLE>(out_handle_), formatted.data() + start, size, nullptr, nullptr);
        (void)(ignored);
    }
}
#else
/**
* This sink prefixes the output with an ANSI escape sequence color code
* depending on the severity
* of the message.
* If no color terminal detected, omit the escape codes.
*/
template<typename ConsoleMutex>
class ansicolor_sink 
{
public:
    using mutex_t = typename ConsoleMutex::mutex_t;
    ansicolor_sink();
    ~ansicolor_sink() = default;

    void set_color(level_enum color_level, string_view_t color);
    void set_color_mode();
    bool should_color();

    void log(const async_msg& msg) ;
    void flush();
    void set_formatter(std::unique_ptr<pattern_formatter> sink_formatter);

    // Formatting codes
    const string_view_t reset = "\033[m";    
    // Foreground colors
    const string_view_t green = "\033[32m";    
    /// Bold colors
    const string_view_t red_bold = "\033[31m\033[1m";
    const string_view_t bold_on_red = "\033[1m\033[41m";

private:
    FILE* target_file_;
    mutex_t& mutex_;
    bool should_do_colors_;
    std::unique_ptr<pattern_formatter> formatter_;
    std::array<std::string, level_enum::n_levels> colors_;
    void print_ccode_(const string_view_t& color_code);
    void print_range_(const memory_buf_t& formatted, size_t start, size_t end);
    static std::string to_string_(const string_view_t& sv);
};

template<typename ConsoleMutex>
ansicolor_sink<ConsoleMutex>::ansicolor_sink()
    : target_file_(stdout)
    , mutex_(ConsoleMutex::mutex())
    , formatter_(std::make_unique<pattern_formatter>())
{
    set_color_mode();
    colors_[level_enum::info] = to_string_(green);
    colors_[level_enum::err] = to_string_(red_bold);
    colors_[level_enum::critical] = to_string_(bold_on_red);
    colors_[level_enum::off] = to_string_(reset);
}

template<typename ConsoleMutex>
void ansicolor_sink<ConsoleMutex>::set_color(level_enum color_level, string_view_t color)
{
    std::lock_guard<mutex_t> lock(mutex_);
    colors_[color_level] = to_string_(color);
}

template<typename ConsoleMutex>
void ansicolor_sink<ConsoleMutex>::log(const async_msg& msg)
{
    // Wrap the originally formatted message in color codes.
    // If color is not supported in the terminal, log as is instead.
    std::lock_guard<mutex_t> lock(mutex_);
    msg.color_range_start = 0;
    msg.color_range_end = 0;
    memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if (should_do_colors_ && msg.color_range_end > msg.color_range_start)
    {
        // before color range
        print_range_(formatted, 0, msg.color_range_start);
        // in color range
        print_ccode_(colors_[msg.level]);
        print_range_(formatted, msg.color_range_start, msg.color_range_end);
        print_ccode_(reset);
        // after color range
        print_range_(formatted, msg.color_range_end, formatted.size());
    }
    else // no color
    {
        print_range_(formatted, 0, formatted.size());
    }
    fflush(target_file_);
}

template<typename ConsoleMutex>
void ansicolor_sink<ConsoleMutex>::flush()
{
    std::lock_guard<mutex_t> lock(mutex_);
    fflush(target_file_);
}

template<typename ConsoleMutex>
void ansicolor_sink<ConsoleMutex>::set_formatter(std::unique_ptr<pattern_formatter> sink_formatter)
{
    std::lock_guard<mutex_t> lock(mutex_);
    formatter_ = std::move(sink_formatter);
}

template<typename ConsoleMutex>
bool ansicolor_sink<ConsoleMutex>::should_color()
{
    return should_do_colors_;
}

template<typename ConsoleMutex>
void ansicolor_sink<ConsoleMutex>::set_color_mode()
{
    should_do_colors_ = ::isatty(fileno(target_file_)) != 0;
}

template<typename ConsoleMutex>
void ansicolor_sink<ConsoleMutex>::print_ccode_(const string_view_t& color_code)
{
    fwrite(color_code.data(), sizeof(char), color_code.size(), target_file_);
}

template<typename ConsoleMutex>
void ansicolor_sink<ConsoleMutex>::print_range_(const memory_buf_t& formatted, size_t start, size_t end)
{
    fwrite(formatted.data() + start, sizeof(char), end - start, target_file_);
}

template<typename ConsoleMutex>
std::string ansicolor_sink<ConsoleMutex>::to_string_(const string_view_t& sv)
{
    return std::string(sv.data(), sv.size());
}
#endif


#ifdef _WIN32
using sink_ptr = std::shared_ptr<wincolor_sink<console_mutex>>;
#else
using sink_ptr = std::shared_ptr<ansicolor_sink<console_mutex>>;
#endif

using sinks_init_list = std::initializer_list<sink_ptr>;

class async_logger : public std::enable_shared_from_this<async_logger>
{
    using item_type = async_msg;
    using q_type = moodycamel::ConcurrentQueue<item_type>;

public:
    template<typename It>
    async_logger(std::string logger_name, It begin, It end)
        : name_(std::move(logger_name))
        , sinks_(begin, end)
        , thread_(&async_logger::worker_loop_, this)
        ,q_(8192U)
    {}
    async_logger(std::string logger_name, sinks_init_list sinks_list);
    async_logger(std::string logger_name, sink_ptr single_sink);

    ~async_logger()
    {
        thread_.join();
    }

    template<typename... Args>
    void info(fmt::format_string<Args...> fmt, Args &&...args)
    {
        log_(level_enum::info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(fmt::format_string<Args...> fmt, Args &&...args)
    {
        log_(level_enum::err, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void critical(fmt::format_string<Args...> fmt, Args &&...args)
    {
        log_(level_enum::critical, fmt, std::forward<Args>(args)...);
    }

    // set formatting for the sinks in this logger.
    // each sink will get a separate instance of the formatter object.
    void set_formatter(std::unique_ptr<pattern_formatter> f);

    // flush functions
    void flush_on(level_enum log_level);
protected:
    void backend_sink_it_(const async_msg& incoming_log_msg);
    void backend_flush_();

    std::string name_;
    std::vector<sink_ptr> sinks_;
    level_t flush_level_{ level_enum::off };

    // common implementation for after templated public api has been resolved
    template<typename... Args>
    void log_(level_enum lvl, string_view_t fmt, Args &&...args)
    {
        memory_buf_t buf;
        fmt::detail::vformat_to(buf, fmt, fmt::make_format_args(args...));
        async_msg log_msg(name_, lvl, string_view_t(buf.data(), buf.size()));
        log_it_(log_msg);
    }
    // log the given message (if the given log level is high enough),
    void log_it_(const async_msg& log_msg);
    void sink_it_(const async_msg& msg);
    void flush_();
    bool should_flush_(const async_msg& msg);

private:
    q_type q_;
    std::thread thread_;

    void post_async_msg_(async_msg&& new_msg);
    void worker_loop_();

    // process next message in the queue
    // return true if this thread should still be active (while no terminate msg
    // was received)
    bool process_next_msg_();
};

struct async_factory
{
    template<typename Sink, typename... SinkArgs>
    static std::shared_ptr<async_logger> create(std::string logger_name, SinkArgs &&...args)
    {
        auto sink = std::make_shared<Sink>(std::forward<SinkArgs>(args)...);
        auto new_logger = std::make_shared<async_logger>(std::move(logger_name), std::move(sink));

        new_logger->flush_on(level_enum::off);

        new_logger->set_formatter(std::make_unique<pattern_formatter>());
        return new_logger;
    }
};

template<typename Factory = async_factory>
std::shared_ptr<async_logger> stdout_color_mt(const std::string& logger_name)
{
#ifdef _WIN32
    return Factory::template create< wincolor_sink<console_mutex> >(logger_name);
#else
    return Factory::template create< ansicolor_sink<console_mutex> >(logger_name);
#endif
}
}