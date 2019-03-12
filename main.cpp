/**
 * @author a.chouak
 * @file   main.cpp
 * @brief  Multi-thread safe resource monitoring infrastructure for the ATLAS experiment - Exercise
 *
 */

#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <cmath>
#include <random>

using namespace std::literals::chrono_literals;

/**
 * @brief pthread utilities
 *
 */
class pthread_utils
{
    public:
        /**
         *
         * @param handle thread native handle
         * @param[out] ts thread cpu time
         * @return true if successful
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
 * @brief feature-augmenting wrapper for std::thread
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

        /**
         *
         * @return the wall time of the thread since instantiation
         */
        std::chrono::duration<double> get_wall_time() const {
            return std::chrono::system_clock::now() - _wall_time_start;
        }

        /**
         *
         * @return the cpu time of the thread
         */
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

        std::thread::native_handle_type native_handle() {
            return _thread.native_handle();
        }

        void detach() const {
            _thread.detach();
        }

        void join() const {
            _thread.join();
        }

        bool joinable() const {
            return _thread.joinable();
        }

        std::thread::id get_id() const {
            return _thread.get_id();
        }
};

const thread_ex::no_callback_t thread_ex::no_callback = nullptr;

class thread_pool_monitor; // Forward declaration

/**
 * @brief fixed-size pool of threads
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
         * Callback for `thread_ex`. This is invoked once the thread is completed
         *
         * @param thread_idx thread index in `_threads`
         */
        void on_thread_completed(pool_size_t thread_idx)
        {
            _completed[thread_idx] = true;
        }

    public:
        explicit thread_pool(pool_size_t size = default_pool_size)
                : _threads(size)
                , _completed(size)
                , _wall_time_start(std::chrono::system_clock::now())
                , _owner_thread_handle(pthread_self())
        {
        }

        thread_pool(thread_pool const& tp) = delete;

        virtual ~thread_pool() = default;

        /**
         *
         * @tparam _Fn type of function to be executed in the newly created thread
         * @tparam _Args parameter pack to be supplied to `_Fn`
         * @param fn instance of `_Fn`
         * @param args instance of `_Args`
         * @return reference to the newly instantiated `thread_ex`
         */
        template<typename _Fn, typename ..._Args>
        thread_ex& enqueue(_Fn&& fn, _Args&&... args)
        {
            std::lock_guard<std::mutex> lk(_threads_mtx);

            if (_next_slot_idx == capacity())
                throw std::runtime_error("Thread pool size exceeds capacity");

            thread_ex tex(std::forward<decltype(fn)>(fn),
                          thread_ex::no_callback,
                          std::forward<decltype(args)>(args)...);
            _threads[_next_slot_idx] = std::move(tex);
            _completed[_next_slot_idx] = false;

            return _threads[_next_slot_idx++];
        }

        /**
         *
         * @ref enqueue(_Fn, _Args)
         */
        template<typename _Fn>
        thread_ex& enqueue(_Fn&& fn)
        {
            std::lock_guard<std::mutex> lk(_threads_mtx);

            if (_next_slot_idx == capacity())
                throw std::runtime_error("Thread pool size exceeds capacity");

            thread_ex tex(std::forward<decltype(fn)>(fn), thread_ex::no_callback);
            _threads[_next_slot_idx] = std::move(tex);
            _completed[_next_slot_idx] = false;

            return _threads[_next_slot_idx++];
        }

        /**
         *
         * @return wall time elapsed since creation
         */
        std::chrono::duration<double> get_wall_time() const {
            return std::chrono::system_clock::now() - _wall_time_start;
        }

        /**
         *
         * @return cpu time consumed in the owner thread since creation
         */
        std::chrono::duration<double> get_cpu_time() const
        {
            timespec now = {0};
            timespec ts = {0};

            if (pthread_utils::get_current_cpu_timespec(_owner_thread_handle, now))
                ts = now;

            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds { ts.tv_sec } +
                                                                         std::chrono::nanoseconds { ts.tv_nsec });
        }

        /**
         *
         * @return the number of instantiated threads
         */
        pool_size_t size() const {
            return _next_slot_idx;
        }

        /**
         *
         * @return the maximum number of instantiable threads
         */
        pool_size_t capacity() const {
            return static_cast<pool_size_t >(_threads.size());
        }

        /**
         *
         * @param i the index of the thread in the pool
         * @return true if the i-th thread is completed
         */
        bool completed(pool_size_t i) const {
            return _completed[i];
        }

        /**
         *
         * @return a constant reference to the thread pool container
         */
        std::vector<thread_ex> const& get_threads() const {
            return _threads;
        }
};

/**
 * @brief resource usage monitor for `thread_pool`
 *
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

        /**
         * @brief periodically polls for the status of threads in `_pool`
         */
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

        /**
         * @brief pauses the monitor watchdog
         */
        void pause() {
            _running = false;
        }

        /**
         * @brief resumes the monitor watchdog
         */
        void resume() {
            _running = true;
        }

        /**
         *
         * @return true if the monitor watchdog is running
         */
        bool running() {
            return _running;
        }

        /**
         *
         * @param probe a `probe_fn_t` thread pool prober
         */
        void add_probe(probe_fn_t const& probe) {
            std::lock_guard<std::mutex> lk(_probe_chain_mtx);

            _probe_chain.push_back(probe);
        }
};

/**
 *
 * @param interval radius interval
 * @return the montecarlo approximation of pi
 */
double montecarlo_pi(int interval)
{
    double rand_x, rand_y, origin_dist, pi;
    int circle_points = 0, square_points = 0;

    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    std::mt19937 mersenne { static_cast<unsigned long>(now.count()) };

    for (int i = 0; i < interval * interval; i++)
    {
        rand_x = double(mersenne() % (interval + 1)) / interval;
        rand_y = double(mersenne() % (interval + 1)) / interval;

        origin_dist = rand_x * rand_x + rand_y * rand_y;

        if (origin_dist <= 1)
            circle_points++;

        square_points++;

        pi = double(4 * circle_points) / square_points;
    }

    return pi;
}

/**
 *
 * @param tp thread pool to be probed
 */
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
        std::cout << "[+] Thread " << id << " - CPU Time (s): " << threads[id].get_cpu_time().count() << std::endl;
    }
}

int main(int argc, char** argv)
{
    thread_pool tp;
    std::atomic<bool> leave_barrier { false };

    // Start the montecarlo workers
    for (int i = 1; i <= 4; i++) {
        tp.enqueue([&leave_barrier](int i) {
            while (!leave_barrier) {
                montecarlo_pi(i);
            }
        }, pow(5, i));
    }

    // Start the monitor
    thread_pool_monitor tpm(tp, { time_probe_fn });

    // Wait for input
    getchar();

    // Raise barrier and join
    leave_barrier = true;
    auto const& threads = tp.get_threads();

    for (auto i = 0; i < tp.size(); i++)
        threads[i].join();

    return 0;
}