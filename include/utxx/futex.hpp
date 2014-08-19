//----------------------------------------------------------------------------
/// \file  futex.hpp
//----------------------------------------------------------------------------
/// \brief Concurrent futex notification.
/// Futex class is an enhanced C++ version of Rusty Russell's furlock
/// interface found in:
/// http://www.kernel.org/pub/linux/kernel/people/rusty/futex-2.2.tar.gz
//----------------------------------------------------------------------------
// Copyright (c) 2011 Serge Aleynikov <saleyn@gmail.com>
// Created: 2011-09-10
//----------------------------------------------------------------------------
/*
***** BEGIN LICENSE BLOCK *****

This file is part of the utxx open-source project.

Copyright (C) 2011 Serge Aleynikov <saleyn@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

***** END LICENSE BLOCK *****
*/
#ifndef _UTXX_FUTEX_HPP_
#define _UTXX_FUTEX_HPP_

#if __cplusplus >= 201103L

#include <limits.h>
#include <errno.h>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>

//-----------------------------------------------------------------------------

namespace utxx {

enum class wakeup_result {
    ERROR    = -1, ///< Some other error
    SIGNALED =  0, ///< Woken by a FUTEX_WAKE call
    CHANGED  =  1, ///< Value changed before FUTEX_WAIT call
    TIMEDOUT =  2  ///< Timed out
};

/// Convert wakeup_result to string
const char* to_string(wakeup_result res);

/** Fast futex-based concurrent notification primitive.
 * Supports signal/wait semantics.
 */
class futex {
    std::atomic<int> m_count;
    #ifdef PERF_STATS
    unsigned long m_wait_count;
    unsigned long m_wake_count;
    unsigned long m_wake_signaled_count;
    unsigned long m_wait_fast_count;
    unsigned long m_wake_fast_count;
    unsigned long m_wait_spin_count;
    #endif

    static const int FUTEX_PASSED = -(1 << 30);

    /// @return -1 - fail, 0 - wakeup, 1 - pass, 2 - didn't sleep
    wakeup_result wait_slow  (int val, const timespec* rel = 0);
    int        signal_slow(int count = 1);

    /// Atomic dec of internal counter.
    /// @return CHANGED  when value is different from old_value
    ///                  or someone updated it during wait_fast call.
    ///         TIMEDOUT when the thread should wait for another signal
    wakeup_result wait_fast(int* old_value = NULL) {
        int val = value();

        if (old_value && *old_value != val) {
            if (old_value)
                *old_value = val;

            #ifdef PERF_STATS
            ++m_wait_fast_count;
            #endif

            return wakeup_result::CHANGED;
        }

        int res = m_count.exchange(0, std::memory_order_acq_rel);

        if (res == 0)
            return wakeup_result::TIMEDOUT;

        #ifdef PERF_STATS
        ++m_wait_fast_count;
        #endif

        return res == val ? wakeup_result::SIGNALED : wakeup_result::CHANGED;
    }

    /// Atomic inc
    /// @return Old value of the counter. If it is 0, it means that
    ///         the consumer likely is waiting.
    int signal_fast() {
        // r = ++m_count >= 1 ? 1 : 0;
        // Note: fetch_add returns old value
        return m_count.fetch_add(1, std::memory_order_relaxed);
    }

    void commit(int n) {
        m_count.store(n, std::memory_order_relaxed);
    }

public:
    futex(int initialize = 1);

    int  value() const  { return m_count.load(std::memory_order_relaxed); }
    int  reset(int a_init = 1) { commit(a_init); return a_init; }

    #ifdef PERF_STATS
    unsigned long wake_count()           const { return m_wake_count;          }
    unsigned long wake_signaled_count()  const { return m_wake_signaled_count; }
    unsigned long wait_count()           const { return m_wait_count;          }
    unsigned long wake_fast_count()      const { return m_wake_fast_count;     }
    unsigned long wait_fast_count()      const { return m_wait_fast_count;     }
    unsigned long wait_spin_count()      const { return m_wait_spin_count;     }
    #endif

    /// Signal the futex by incrementing the internal
    /// variable and optionally making a system call.
    /// @return Number of processes woken up
    int signal(int count_to_wake = 1) {
        if (!signal_fast()) // Someone might be waiting
            return signal_slow(count_to_wake);
        #ifdef PERF_STATS
        ++m_wake_fast_count;
        #endif
        return 0;
    }

    /// Signal all waiting threads.
    /// @return number of processes woken up.
    int signal_all() { return signal_slow(INT_MAX); };

    /// Non-blocking attempt to wait for signal
    /// @return 0 - success, -1 - no pending signal
    int try_wait(int* old_val = NULL) {
        return wait_fast(old_val) == wakeup_result::SIGNALED ? 0 : -1;
    }

    /// Wait for signaled condition up to \a timeout. Note that
    /// the call ignores spurious wakeups.
    /// @param <old_val> - pointer to the old <value()> of futex
    ///         known just before calling <wait()> function.
    /// @return SIGNALED    - woken up or value changed before sleep
    ///         CHANGED     - if value changed before futex_wait call
    ///         ERROR       - timeout or some other error occured
    ///         TIMEDOUT    - timed out
    wakeup_result wait(int* old_val = NULL) { return wait(NULL, old_val); }

    /// Wait for signaled condition up to \a timeout. Note that
    /// the call ignores spurious wakeups.
    /// @param <timeout> - max time to wait (NULL means infinity)
    /// @param <old_val> - pointer to the old <value()> of futex
    ///         known just before calling <wait()> function.
    /// @return SIGNALED    - woken up or value changed before sleep
    ///         CHANGED     - if value changed before futex_wait call
    ///         ERROR       - timeout or some other error occured
    ///         TIMEDOUT    - timed out
    wakeup_result wait(const struct timespec *timeout, int* old_val = NULL);

    /// Wait for signaled condition until \a wait_until_abs_time.
    /// \copydetails wait()
    wakeup_result wait
    (
        const std::chrono::milliseconds& wait_duration,
        int* old_val = NULL
    ) {
        using namespace std::chrono;
        auto nsec = duration_cast<std::chrono::nanoseconds>(wait_duration);
        struct timespec ts = { nsec.count() / 1000000000L,
                               nsec.count() % 1000000000L };
        return wait(&ts, old_val);
    }

};

} // namespace utxx

#endif // __cplusplus

#endif // _UTXX_FUTEX_HPP_
