/**
 * @author a.chouak
 * @file   main.cpp
 * @brief  Multi-thread safe resource monitoring infrastructure for the ATLAS experiment - Exercise
 *
 */

#include <atomic>
#include <thread>
#include <functional>
#include <chrono>
#include <ctime>
#include <mutex>
#include <vector>
#include <iostream>
#include <unordered_map>

using namespace std::literals::chrono_literals;

/**
 * @class Pthread Utilities
 *
 */
class pthread_utils
{
    public:
        /**
         *
         * @param handle thread native handle
         * @param[out] ts thread cpu time
         * @return a boolean indicating success
         */
        static bool get_current_cpu_timespec(std::thread::native_handle_type handle, timespec& ts)
        {
            clockid_t clock_id;

            if (pthread_getcpuclockid(handle, &clock_id) == 0)
            {
                if (clock_gettime(clock_id, &ts) == 0)
                    return true;
            }

            return false;
        }
};

/**
 * @class A thread class that wraps std::thread and extends its functionalities
 *
 */
class thread_ex
{
    public:
        using no_callback_t = std::nullptr_t;
        static const no_callback_t no_callback;

    protected:
        mutable std::thread _thread;
        mutable timespec _last_cpu_time = {0};
        std::chrono::system_clock::time_point _wall_time_start;

        template<typename _CbFn>
        typename std::enable_if<std::is_same<typename std::decay<_CbFn>::type, no_callback_t>::value>::type
            invoke_callback(_CbFn&& cb_fn)
        {

        }

        template<typename _CbFn>
        typename std::enable_if<!std::is_same<typename std::decay<_CbFn>::type, no_callback_t>::value>::type
            invoke_callback(_CbFn&& cb_fn)
        {
            cb_fn();
        }

        template<typename _Fn, typename _CbFn, typename ..._Args>
        auto make_thread_thunk(_Fn&& fn, _CbFn&& cb_fn, _Args&&... args)
        {
            return [&]()
            {
                fn(args...);

                // Callback
                invoke_callback(std::forward<decltype(cb_fn)>(cb_fn));
            };
        }

        template<typename _Fn, typename _CbFn>
        auto make_thread_thunk(_Fn&& fn, _CbFn&& cb_fn)
        {
            return [&]()
            {
                fn();

                // Callback
                invoke_callback(std::forward<decltype(cb_fn)>(cb_fn));
            };
        }

    public:
        thread_ex() = default;

        thread_ex(thread_ex&& other) noexcept
                : _wall_time_start(other._wall_time_start)
                , _thread(std::move(other._thread))
        {

        }

        template<typename _Fn, typename _CbFn, typename ..._Args>
        explicit thread_ex(_Fn&& fn, _CbFn&& cb_fn, _Args&&... args)
                : _wall_time_start(std::chrono::system_clock::now())
                , _thread(make_thread_thunk(std::forward<decltype(fn)>(fn),
                                            std::forward<decltype(cb_fn)>(cb_fn),
                                            std::forward<decltype(args)>(args)...))
        {
        }

        template<typename _Fn, typename _CbFn>
        explicit thread_ex(_Fn&& fn, _CbFn&& cb_fn)
                : _wall_time_start(std::chrono::system_clock::now())
                , _thread(make_thread_thunk(std::forward<decltype(fn)>(fn),
                                            std::forward<decltype(cb_fn)>(cb_fn)))
        {
        }

        thread_ex& operator=(thread_ex&& other) noexcept
        {
            _thread = std::move(other._thread);
            _wall_time_start = other._wall_time_start;
        }

        void detach() {
            _thread.detach();
        }

        void join() {
            _thread.join();
        }

        bool joinable() {
            return _thread.joinable();
        }

        std::thread::id get_id() {
            return _thread.get_id();
        }

        auto native_handle() {
            return _thread.native_handle();
        }

        std::chrono::duration<double> get_wall_time() const {
            return std::chrono::system_clock::now() - _wall_time_start;
        }

        std::chrono::duration<double> get_cpu_time() const
        {
            timespec now = {0};
            timespec ts = {0};

            if (pthread_utils::get_current_cpu_timespec(_thread.native_handle(), now))
                _last_cpu_time = ts = now;
            else
                ts = _last_cpu_time;

            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds { ts.tv_sec } +
                                                                         std::chrono::nanoseconds { ts.tv_nsec });
        }
};

const thread_ex::no_callback_t thread_ex::no_callback = nullptr;

class thread_pool_monitor;

/**
 * @class Thread Pool
 *
 */
class thread_pool
{
    friend class thread_pool_monitor;

    protected:
        using pool_size_t = unsigned int;

        static constexpr unsigned int default_pool_size = 32;

        std::vector<thread_ex> _threads;
        std::vector<std::atomic<bool>> _completed;
        std::chrono::system_clock::time_point _wall_time_start;
        std::thread::native_handle_type _owner_thread_handle;
        pool_size_t _next_slot_idx = 0;

        mutable std::mutex _threads_mtx;

        /**
         * Callback for a `thread_ex`. This is invoked once the thread is completed
         *
         * @param thread_idx thread index in `_threads`
         */
        void on_thread_completed(pool_size_t thread_idx)
        {
            _completed[thread_idx] = true;
        }

    public:
        thread_pool()
            : _threads(default_pool_size)
            , _completed(default_pool_size)
            , _wall_time_start(std::chrono::system_clock::now())
            , _owner_thread_handle(pthread_self())
        {
        }

        explicit thread_pool(pool_size_t size)
                : _threads(size)
                , _completed(size)
                , _wall_time_start(std::chrono::system_clock::now())
                , _owner_thread_handle(pthread_self())
        {
        }

        thread_pool(thread_pool const& tp) = delete;

        virtual ~thread_pool() = default;

        template<typename _Fn, typename ..._Args>
        thread_ex& enqueue(_Fn&& fn, _Args&&... args)
        {
            std::lock_guard<std::mutex> lk(_threads_mtx);

            // @todo: Throw exception when overflowing the pool
            thread_ex tex(std::forward<decltype(fn)>(fn),
                          thread_ex::no_callback,
                          std::forward<decltype(args)>(args)...);
            _threads[_next_slot_idx] = std::move(tex);
            _completed[_next_slot_idx] = false;

            return _threads[_next_slot_idx++];
        }

        template<typename _Fn>
        thread_ex& enqueue(_Fn&& fn)
        {
            std::lock_guard<std::mutex> lk(_threads_mtx);

            // @todo: Throw exception when overflowing the pool
            thread_ex tex(std::forward<decltype(fn)>(fn), thread_ex::no_callback);
            _threads[_next_slot_idx] = std::move(tex);
            _completed[_next_slot_idx] = false;

            return _threads[_next_slot_idx++];
        }

        std::chrono::duration<double> get_wall_time() const {
            return std::chrono::system_clock::now() - _wall_time_start;
        }

        std::chrono::duration<double> get_cpu_time() const
        {
            timespec now = {0};
            timespec ts = {0};

            if (pthread_utils::get_current_cpu_timespec(_owner_thread_handle, now))
                ts = now;

            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds { ts.tv_sec } +
                                                                         std::chrono::nanoseconds { ts.tv_nsec });
        }

        pool_size_t size() const
        {
            return _next_slot_idx;
        }

        pool_size_t capacity() const
        {
            return static_cast<pool_size_t >(_threads.size());
        }

        auto const& get_threads() const {
            return _threads;
        }

        auto const& get_completed() const {
            return _completed;
        }
};

/**
 * @class Thread Pool Monitor
 * @todo - Fluent interface for adding probes to the chain
 *        - Probe functions should be indexed (for pnp probes to work)
 */
class thread_pool_monitor
{
    public:
        using probe_fn_t = std::function<void (thread_pool const&)>;

    protected:
        thread_pool const& _pool;
        std::thread _watchdog_thread;
        std::chrono::duration<double> _watchdog_wakeup_timeout;
        std::atomic<bool> _running;
        std::atomic<bool> _disposed;
        std::vector<probe_fn_t> _probe_chain;
        std::mutex _probe_chain_mtx;

        void watchdog()
        {
            while (!_disposed)
            {
                if (_running)
                {
                    std::unique_lock<std::mutex> lk(_probe_chain_mtx);

                    auto _probe_chain_snapshot(_probe_chain);

                    lk.unlock();

                    // Lock the thread pool for probing
                    std::lock_guard<std::mutex> lk_(_pool._threads_mtx);

                    for (auto &probe : _probe_chain_snapshot) {
                        probe(_pool);
                    }

                    std::this_thread::sleep_for(_watchdog_wakeup_timeout);
                }
                else
                    std::this_thread::yield();
            }
        }

    public:
        virtual ~thread_pool_monitor() {
            _disposed = true;
            _watchdog_thread.join();
        }

        explicit thread_pool_monitor(thread_pool const& pool,
                                     std::vector<probe_fn_t> probe_chain,
                                     std::chrono::duration<double> watchdog_wakeup_timeout = 500ms)
                : _pool(pool)
                , _disposed(false)
                , _running(true)
                , _probe_chain(std::move(probe_chain))
                , _watchdog_wakeup_timeout(watchdog_wakeup_timeout)
                , _watchdog_thread([this]() { watchdog(); })
        {

        }

        explicit thread_pool_monitor(thread_pool const& pool,
                           std::chrono::duration<double> watchdog_wakeup_timeout = 500ms)
                : thread_pool_monitor(pool, {}, watchdog_wakeup_timeout)
        {

        }

        void pause() {
            _running = false;
        }

        void resume() {
            _running = true;
        }

        bool running() {
            return _running;
        }

        void add_probe(probe_fn_t const& probe) {
            std::lock_guard<std::mutex> lk(_probe_chain_mtx);

            _probe_chain.push_back(probe);
        }
};

void montecarlo_pi() {
    int interval, i;
    double rand_x, rand_y, origin_dist, pi;
    int circle_points = 0, square_points = 0;
    constexpr int INTERVAL = 10000;

    // Initializing rand()
    srand(time(NULL));

    // Total Random numbers generated = possible x
    // values * possible y values
    for (i = 0; i < (INTERVAL * INTERVAL); i++) {

        // Randomly generated x and y values
        rand_x = double(rand() % (INTERVAL + 1)) / INTERVAL;
        rand_y = double(rand() % (INTERVAL + 1)) / INTERVAL;

        // Distance between (x, y) from the origin
        origin_dist = rand_x * rand_x + rand_y * rand_y;

        // Checking if (x, y) lies inside the define
        // circle with R=1
        if (origin_dist <= 1)
            circle_points++;

        // Total number of points generated
        square_points++;

        // estimated pi after this iteration
        pi = double(4 * circle_points) / square_points;
    }
}

void time_probe_fn(thread_pool const& tp)
{
    auto const& threads = tp.get_threads();
    auto n_threads = tp.size();
    auto pool_capacity = tp.capacity();
    auto cpu_time = tp.get_cpu_time();
    auto wall_time = tp.get_wall_time();

    std::system("clear");
    std::cout << "[+] Thread Pool Size: " << n_threads << std::endl;
    std::cout << "[+] Thread Pool Capacity: " << pool_capacity << std::endl;
    std::cout << "[+] Thread Pool CPU Time (s): " << cpu_time.count() << std::endl;
    std::cout << "[+] Thread Pool Wall Time (s): " << wall_time.count() << std::endl;

    for (auto id = 0; id < n_threads; id++) {
        std::cout << "[+]\tThread " << id << " - CPU Time (s): " << threads[id].get_cpu_time().count() << std::endl;
    }
}

int main(int argc, char** argv)
{
    thread_pool tp;
    thread_pool_monitor tpm(tp, { time_probe_fn });

    thread_ex& t1 = tp.enqueue([]() {
        for (volatile int i = 0; i < 10e8; i++)
            ;
    });
    thread_ex& t2 = tp.enqueue([]() {
        while (true) {
            montecarlo_pi();
        }
    });
    thread_ex& t3 = tp.enqueue([]() {
        while (true) {
            montecarlo_pi();
        }
    });
    thread_ex& t4 = tp.enqueue([]() {
        while (true) {
            montecarlo_pi();
        }
    });
    thread_ex& t5 = tp.enqueue([]() {
        while (true) {
            montecarlo_pi();
        }
    });

    while(true);

    return 0;
}