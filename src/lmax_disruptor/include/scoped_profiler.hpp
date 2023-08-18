#pragma once

#include <vector>
#include <chrono>
#include <mutex>
#include <iostream>
#include <thread>
#include <cmath>
namespace profiler{

    std::mutex lock;

    // For now, this class can only be used in a single thread.
    class ScopedProfiler{
      public:

        static std::vector<double> data;

        ScopedProfiler() { start_time_ = std::chrono::high_resolution_clock::now(); }

        ~ScopedProfiler() {
            auto duration = (std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::high_resolution_clock::now()-start_time_)).count();
            {
                std::scoped_lock<std::mutex>lk (lock); data.push_back(duration);
            }
        }
   
    private:
        using TimePoint     = std::chrono::time_point<std::chrono::high_resolution_clock>;
        TimePoint           start_time_;
     };

    //---------------------------------------------------------------------------
     std::vector<double> ScopedProfiler::data{};

    //---------------------------------------------------------------------------
        struct Stats{
        double mean;
        double max;
        double min;
        double stdev;
    };
    //---------------------------------------------------------------------------
    std::ostream& operator<<(std::ostream& os, const Stats& s) {
        os << "Mean: " <<s.mean<<", Min: "<<s.min << ", Max: "<< s.max<<", Stddev: "<<s.stdev<<'\n';
        return os;
    }
    //---------------------------------------------------------------------------
    template<typename T>
    Stats GetStats(const std::vector<T>& arr) {
        Stats stats{0, arr[0],arr[0], 0};

        for(const T& elem: arr) {
            if (elem < stats.min) {stats.min = elem;}
            if (elem > stats.max) {stats.max = elem;}
            stats.mean += elem;
        }
        stats.mean/=static_cast<double>(arr.size());
        
        for(const T& elem: arr) {
            stats.stdev+=std::pow(stats.mean-elem,2);
        }
        stats.stdev /= static_cast<double>(arr.size());
        stats.stdev = std::sqrt(stats.stdev);      
        return stats;
    }
    //---------------------------------------------------------------------------
    class Timer {
        using TimePoint = std::chrono::time_point<std::chrono::high_resolution_clock>;
      public:
        Timer()         = default;
        void    Start()     { start_time_ = std::chrono::high_resolution_clock::now(); }
        void    Display()   { std::cout << GetStats(data_);  }
        Stats   Stats()     { return GetStats(data_);}
        void    Stop()      {
                            auto duration = (std::chrono::duration_cast<std::chrono::nanoseconds> 
                                (std::chrono::high_resolution_clock::now()-start_time_)).count();
                            data_.push_back(duration);
                            }       
      private:
        std::vector<double>     data_{};
        TimePoint               start_time_;

    };

} //namespace profiler
