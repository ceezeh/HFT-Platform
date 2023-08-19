#include <chrono>
#include <deque>
#include <future>
#include <ostream>
#include <vector>

#include "barrier.hpp"
#include "disruptor.hpp"
#include "scoped_profiler.hpp"

namespace tests{
    bool WAIT_TEST (auto fn) {
        	long timeout = 1e6; //1 second
            long wait_delta = 50; // 50ns
            while (!fn() && timeout > 0) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(wait_delta));
                    timeout-=wait_delta;
            }
	        return fn();
    }

    //---------------------------------------------------------------------------
	template <typename T>
    class ThreadSafeQueue {
    public:
        ThreadSafeQueue() = default;

    std::optional<T> front() {
        barrier::ScopedBarrier b{sync_};
        if (queue_.empty()) return {};
        return queue_.front();
    }

    void Release() {
        barrier::ScopedBarrier b{sync_};
        queue_.pop_front();
    }

    void Insert(T&& data) {
        barrier::ScopedBarrier b{sync_};
        queue_.push_back(std::forward<T>(data));
    }
        
    private:
        std::deque<T> queue_;
        std::atomic<bool> sync_{};
    
    };

    //---------------------------------------------------------------------------
    profiler::Stats TimedDisruptorTask (const size_t NoOfWriters, const size_t NoOfWritesPerWriter, const bool bl_disable = true) {
        using WriterType = disruptor::Writer<size_t, disruptor::PublishPolicy::BLOCK, disruptor::PublishPolicy::BLOCK>;

		auto disruptor = disruptor::MakeSingleDisruptor<size_t, disruptor::PublishPolicy::BLOCK, disruptor::PublishPolicy::BLOCK>();
		std::vector<WriterType>  writers;
		writers.reserve(NoOfWriters);

		for (size_t i = 0; i < NoOfWriters; ++i) 
		{
			writers.push_back(disruptor.CreateWriter());
		}


        auto loop = [&] (WriterType& writer, const size_t data) {
            
            size_t data_t = data;
            for (size_t i = 0; i < NoOfWritesPerWriter; ++i) {	
                size_t d = data_t;
                if (bl_disable) {
                    while(writer.Write(std::forward<size_t>(d))) {}	
                }
                				
                ++data_t;
            }
        };

        std::vector<std::future<void>> futures(NoOfWriters);
        {
            size_t start_count  = 0;
            for (size_t i = 0; i < NoOfWriters; ++i) {
                futures[i] = std::async(std::launch::async, loop, std::ref(writers[i]), start_count);
                start_count += NoOfWritesPerWriter;
            }
        }

        auto reader = disruptor.CreateReader();
        // Read and store.
        std::vector<size_t> sink;
        sink.reserve(NoOfWriters*NoOfWritesPerWriter);
        profiler::Timer timer;
        
        timer.Start();
        if (bl_disable) {
            while (sink.size() < NoOfWriters*NoOfWritesPerWriter) 
            {
                auto read_result = reader.Read(128);
                if (read_result.err) {continue;}
                for (auto iter = read_result.begin; iter != read_result.end; ++iter) 
                {
                    sink.push_back((*iter).data());
                }
                read_result.Release();
            }
        }
        timer.Stop();

        for (size_t i = 0; i < NoOfWriters; ++i) 
        {
            if(futures[i].valid()) 
                futures[i].wait();
        }

        // Process time.
        return timer.Stats();
    }

    //---------------------------------------------------------------------------
    profiler::Stats TimedQueueTask (const size_t NoOfWriters, const size_t NoOfWritesPerWriter) {
        
        ThreadSafeQueue<size_t> q;

        auto loop = [&] (const size_t data) {
            size_t data_t = data;
            for (size_t i = 0; i < NoOfWritesPerWriter; ++i) {	
                size_t d = data_t;
                q.Insert(std::forward<size_t>(d));				
                ++data_t;
            }
        };


        std::vector<std::future<void>> futures(NoOfWriters);
        {
            size_t start_count  = 0;
            for (size_t i = 0; i < NoOfWriters; ++i) {
                futures[i] = std::async(std::launch::async, loop, start_count);
                start_count += NoOfWritesPerWriter;
            }
        }


        std::vector<size_t> sink;
        sink.reserve(NoOfWriters*NoOfWritesPerWriter);
        profiler::Timer timer;


        timer.Start();
        while (sink.size() < NoOfWriters*NoOfWritesPerWriter) 
        {
            auto read_result = q.front();
            if (!read_result) {continue;}
            sink.push_back(*read_result);
            q.Release();
        }
        timer.Stop();

        for (size_t i = 0; i < NoOfWriters; ++i) 
        {
            if(futures[i].valid()) 
                futures[i].wait();
        }

        // Process time.
        return timer.Stats();
    }
}
