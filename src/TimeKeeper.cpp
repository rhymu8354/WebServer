/**
 * @file TimeKeeper.cpp
 *
 * This module contains the implementations of the TimeKeeper class.
 *
 * © 2018 by Richard Walters
 */

#include "TimeKeeper.hpp"

#include <SystemAbstractions/Time.hpp>

/**
 * This contains the private properties of a TimeKeeper class instance.
 */
struct TimeKeeper::Impl {
    /**
     * This is used to interface with the operating system's notion of time.
     */
    SystemAbstractions::Time time;
};

TimeKeeper::~TimeKeeper() noexcept = default;

TimeKeeper::TimeKeeper()
    : impl_(new Impl())
{
}

double TimeKeeper::GetCurrentTime() {
    static double startTime = impl_->time.GetTime();
    return impl_->time.GetTime() - startTime;
}
